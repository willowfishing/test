#pragma once

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/index_runtime.h"
#include "execution_common.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class MinMaxIndexAggregateExecutor : public AbstractExecutor {
   private:
    struct PrefixCond {
        int key_offset = 0;
        int len = 0;
        const char *rhs_value = nullptr;
    };

    SmManager *sm_manager_ = nullptr;
    std::string tab_name_;
    std::string visible_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    IxIndexHandle *ih_ = nullptr;
    RmFileHandle *fh_ = nullptr;
    std::vector<ColMeta> full_cols_;
    ColMeta agg_input_col_;
    ColMeta output_col_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    ast::AggType agg_type_ = ast::AGG_MIN;

    std::vector<PrefixCond> compiled_prefix_conds_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    int equality_prefix_len_ = 0;
    std::string index_key_scratch_;
    std::string visible_key_scratch_;

    RmRecordPageCursor data_page_cursor_;
    ReadCommittedVisibilityCursor rc_visibility_;
    RmRecord current_record_;
    RmRecord output_tuple_;
    size_t cursor_ = 0;
    bool has_tuple_ = false;

    bool use_mvcc_visibility() const {
        return context_ != nullptr && UseMvccReadVisibility(context_->txn_);
    }

    void compile_index_prefix() {
        compiled_prefix_conds_.clear();
        equality_prefix_len_ = 0;
        int key_offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            auto cond_it = std::find_if(conds_.begin(), conds_.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                       cond.lhs_col.tab_name == visible_name_ && cond.lhs_col.col_name == index_col.name;
            });
            if (cond_it == conds_.end()) {
                break;
            }
            compiled_prefix_conds_.push_back({key_offset, index_col.len, cond_it->rhs_val.raw->data});
            key_offset += index_col.len;
            equality_prefix_len_ = key_offset;
        }
    }

    bool eval_slot_conds(const char *slot) const {
        RmRecord record_view;
        record_view.data = const_cast<char *>(slot);
        record_view.size = static_cast<int>(full_cols_.empty() ? 0 : full_cols_.back().offset + full_cols_.back().len);
        record_view.allocated_ = false;
        if (compiled_conds_valid_) {
            return eval_compiled_conds_record(&record_view, compiled_conds_);
        }
        return eval_conds(full_cols_, &record_view, conds_);
    }

    bool visible_record_matches_index_key(const Rid &rid, const char *key) {
        if (key == nullptr || current_record_.data == nullptr) {
            return false;
        }
        char *visible_key = rmdb::build_index_key_into(index_meta_, current_record_.data, rid, &visible_key_scratch_);
        return memcmp(visible_key, key, index_meta_.col_tot_len) == 0;
    }

    void write_empty_value() {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        memset(output_tuple_.data, 0, len_);
    }

    void write_value(const char *slot) {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        memcpy(output_tuple_.data + output_col_.offset, slot + agg_input_col_.offset, agg_input_col_.len);
    }

    bool find_min_value() {
        std::fill(index_key_scratch_.begin(), index_key_scratch_.end(), '\0');
        for (const auto &prefix : compiled_prefix_conds_) {
            memcpy(index_key_scratch_.data() + prefix.key_offset, prefix.rhs_value, prefix.len);
        }
        int key_offset = equality_prefix_len_;
        for (int i = static_cast<int>(compiled_prefix_conds_.size()); i < index_meta_.col_num; ++i) {
            const auto &col = index_meta_.cols[i];
            if (col.type == TYPE_INT) {
                int min_value = std::numeric_limits<int>::min();
                memcpy(index_key_scratch_.data() + key_offset, &min_value, sizeof(int));
            } else if (col.type == TYPE_FLOAT) {
                float min_value = -std::numeric_limits<float>::max();
                memcpy(index_key_scratch_.data() + key_offset, &min_value, sizeof(float));
            }
            key_offset += col.len;
        }

        IxReadGuard read_guard = ih_->make_read_guard();
        Iid lower = equality_prefix_len_ > 0 ? ih_->lower_bound(index_key_scratch_.data()) : ih_->leaf_begin();
        Iid upper = ih_->leaf_end();
        auto scan = ih_->create_scan(lower, upper);
        while (scan != nullptr && !scan->is_end()) {
            if (equality_prefix_len_ > 0 && memcmp(scan->key(), index_key_scratch_.data(), equality_prefix_len_) != 0) {
                break;
            }
            Rid rid = scan->rid();
            if (context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid, context_->txn_, &current_record_) &&
                visible_record_matches_index_key(rid, scan->key())) {
                if (eval_conds(full_cols_, &current_record_, conds_)) {
                    write_value(current_record_.data);
                    return true;
                }
            }
            scan->next();
        }
        return false;
    }

   public:
    MinMaxIndexAggregateExecutor(SmManager *sm_manager,
                                 std::string tab_name,
                                 std::string visible_name,
                                 std::vector<Condition> conds,
                                 std::vector<std::string> index_col_names,
                                 TabCol agg_col,
                                 ast::AggType agg_type,
                                 std::vector<TabCol> output_cols,
                                 Context *context,
                                 std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);
        agg_type_ = agg_type;

        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = use_cache ? *runtime_cache->tab : sm_manager_->db_.get_table(tab_name_);
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(tab_.get_index_meta(index_col_names_, true));
        ih_ = runtime_cache != nullptr && runtime_cache->has_index
                  ? runtime_cache->ih
                  : rmdb::resolve_index_handle(sm_manager_, tab_name_, index_meta_);
        fh_ = use_cache ? runtime_cache->fh : sm_manager_->fhs_.at(tab_name_).get();
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
        } else {
            full_cols_ = tab_.cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible_name_;
            }
        }
        agg_input_col_ = *get_col(full_cols_, agg_col);

        output_col_.tab_name = "";
        output_col_.name = output_cols.empty() ? agg_col.col_name : output_cols[0].col_name;
        output_col_.offset = 0;
        output_col_.type = agg_input_col_.type;
        output_col_.len = agg_input_col_.len;
        cols_.push_back(output_col_);
        len_ = static_cast<size_t>(output_col_.len);

        index_key_scratch_.resize(index_meta_.col_tot_len);
        visible_key_scratch_.resize(index_meta_.col_tot_len);
        data_page_cursor_.bind(fh_);
        rc_visibility_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                            context_ == nullptr ? nullptr : context_->txn_, tab_name_);
        compile_index_prefix();
        compiled_conds_valid_ = compile_conds(full_cols_, conds_, &compiled_conds_);
    }

    void beginTuple() override {
        cursor_ = 0;
        has_tuple_ = true;
        rc_visibility_.reset();
        data_page_cursor_.reset();
        if (use_mvcc_visibility() || agg_type_ != ast::AGG_MIN) {
            has_tuple_ = false;
            return;
        }
        if (!find_min_value()) {
            write_empty_value();
        }
    }

    void nextTuple() override {
        if (cursor_ == 0) {
            cursor_ = 1;
        }
    }

    bool is_end() const override { return !has_tuple_ || cursor_ > 0; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "MinMaxIndexAggregateExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, output_tuple_.data, len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        return is_end() ? nullptr : &output_tuple_;
    }
};
