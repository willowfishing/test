#pragma once

#include <algorithm>

#include "common/index_runtime.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::string right_tab_name_;
    std::string right_visible_name_;
    TabMeta right_tab_;
    RmFileHandle *right_fh_;
    RmRecordPageCursor right_page_cursor_;
    IndexMeta index_meta_;
    std::vector<Condition> fed_conds_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> right_full_cols_;
    std::vector<ColMeta> right_output_cols_;
    std::vector<ColMeta> right_output_source_cols_;
    std::vector<ColMeta> eval_cols_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    size_t left_len_;
    size_t right_len_;
    size_t right_full_len_;
    size_t len_;
    SmManager *sm_manager_;
    Context *context_;
    IxIndexHandle *ih_{nullptr};

    bool isend_ = false;
    AbstractExecutor::TupleViewRef left_tuple_;
    std::vector<Rid> matched_rids_;
    size_t cursor_ = 0;
    bool has_current_ = false;
    mutable RmRecord current_record_;
    RmRecord current_right_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;
    std::string index_key_scratch_;

    int build_key_from_left_view(const TupleView &left_view, char *key) {
        int offset = 0;
        const auto &left_cols = left_->cols();
        for (auto &index_col : index_meta_.cols) {
            bool found = false;
            for (auto &cond : fed_conds_) {
                if (cond.op != OP_EQ || cond.is_rhs_val) {
                    continue;
                }
                TabCol left_col;
                if (cond.rhs_col.tab_name == right_visible_name_ && cond.rhs_col.col_name == index_col.name) {
                    left_col = cond.lhs_col;
                } else if (cond.lhs_col.tab_name == right_visible_name_ && cond.lhs_col.col_name == index_col.name) {
                    left_col = cond.rhs_col;
                } else {
                    continue;
                }
                auto left_meta = get_col(left_cols, left_col);
                memcpy(key + offset, left_view.cell_at(*left_meta, static_cast<size_t>(left_meta - left_cols.begin())),
                       index_col.len);
                found = true;
                break;
            }
            if (!found) {
                break;
            }
            offset += index_col.len;
        }
        return offset;
    }

    bool set_current_from_right_record(const RmRecord &right_rec) {
        if (!left_tuple_) {
            return false;
        }
        const auto &left_cols = left_->cols();
        current_cells_.resize(left_cols.size() + right_output_source_cols_.size());
        size_t out_idx = 0;
        for (size_t i = 0; i < left_cols.size(); ++i) {
            current_cells_[out_idx++] = left_tuple_.view->cell_at(left_cols[i], i);
        }
        for (const auto &col : right_output_source_cols_) {
            current_cells_[out_idx++] = right_rec.data + col.offset;
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
        std::vector<const char *> eval_cells;
        eval_cells.reserve(left_cols.size() + right_full_cols_.size());
        for (size_t i = 0; i < left_cols.size(); ++i) {
            eval_cells.push_back(left_tuple_.view->cell_at(left_cols[i], i));
        }
        for (const auto &col : right_full_cols_) {
            eval_cells.push_back(right_rec.data + col.offset);
        }
        TupleView eval_view;
        eval_view.record = nullptr;
        eval_view.cells = &eval_cells;
        return compiled_conds_valid_ ? eval_compiled_conds_view(eval_view, compiled_conds_)
                                     : eval_conds_view(eval_cols_, eval_view, fed_conds_);
    }

    void load_matches_for_left() {
        matched_rids_.clear();
        cursor_ = 0;
        if (left_->is_end()) {
            return;
        }
        left_tuple_ = left_->ReadTupleView();
        if (!left_tuple_) {
            return;
        }
        index_key_scratch_.resize(index_meta_.col_tot_len);
        std::fill(index_key_scratch_.begin(), index_key_scratch_.end(), '\0');
        int prefix_len = build_key_from_left_view(*left_tuple_.view, index_key_scratch_.data());
        if (prefix_len > 0) {
            if (index_meta_.unique && prefix_len == index_meta_.col_tot_len) {
                ih_->get_value(index_key_scratch_.data(), &matched_rids_,
                               context_ == nullptr ? nullptr : context_->txn_);
            } else {
                ih_->get_prefix_values(index_key_scratch_.data(), prefix_len, &matched_rids_);
            }
        }
    }

    bool advance_to_match() {
        while (!left_->is_end()) {
            while (cursor_ < matched_rids_.size()) {
                if (!right_page_cursor_.read_record(matched_rids_[cursor_], &current_right_record_)) {
                    cursor_++;
                    continue;
                }
                cursor_++;
                if (set_current_from_right_record(current_right_record_)) {
                    has_current_ = true;
                    return true;
                }
            }
            left_->nextTuple();
            load_matches_for_left();
        }
        right_page_cursor_.reset();
        return false;
    }

   public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, SmManager *sm_manager,
                                const std::string &right_tab_name, std::vector<Condition> conds,
                                std::vector<std::string> index_col_names, Context *context,
                                std::string right_visible_name = "", std::vector<TabCol> right_required_cols = {},
                                std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr) {
        left_ = std::move(left);
        sm_manager_ = sm_manager;
        context_ = context;
        right_tab_name_ = right_tab_name;
        right_visible_name_ = right_visible_name.empty() ? right_tab_name_ : std::move(right_visible_name);
        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        right_tab_ = use_cache ? *runtime_cache->tab : sm_manager_->db_.get_table(right_tab_name_);
        right_fh_ = use_cache ? runtime_cache->fh : sm_manager_->fhs_.at(right_tab_name_).get();
        right_page_cursor_.bind(right_fh_);
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(right_tab_.get_index_meta(index_col_names, true));
        ih_ = runtime_cache != nullptr && runtime_cache->has_index
                  ? runtime_cache->ih
                  : rmdb::resolve_index_handle(sm_manager_, right_tab_name_, index_meta_);
        fed_conds_ = std::move(conds);

        left_len_ = left_->tupleLen();
        if (use_cache) {
            right_full_cols_ = runtime_cache->full_cols;
            right_full_len_ = runtime_cache->full_len;
        } else {
            right_full_cols_ = right_tab_.cols;
            for (auto &col : right_full_cols_) {
                col.tab_name = right_visible_name_;
            }
            right_full_len_ = right_full_cols_.back().offset + right_full_cols_.back().len;
        }
        if (right_required_cols.empty()) {
            right_output_cols_ = right_full_cols_;
            right_output_source_cols_ = right_full_cols_;
            right_len_ = right_full_len_;
        } else {
            build_column_projection(right_full_cols_, right_required_cols, &right_output_cols_,
                                    &right_output_source_cols_, &right_len_);
            if (right_output_cols_.empty()) {
                right_output_cols_ = right_full_cols_;
                right_output_source_cols_ = right_full_cols_;
                right_len_ = right_full_len_;
            }
        }
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        for (auto &col : right_output_cols_) {
            col.offset += left_len_;
            cols_.push_back(col);
        }
        eval_cols_ = left_->cols();
        auto adjusted_full = right_full_cols_;
        for (auto &col : adjusted_full) {
            col.offset += static_cast<int>(left_len_);
            eval_cols_.push_back(col);
        }
        compiled_conds_valid_ = compile_conds(eval_cols_, fed_conds_, &compiled_conds_);
        current_record_.Resize(static_cast<int>(len_));
    }

    void beginTuple() override {
        isend_ = false;
        has_current_ = false;
        right_page_cursor_.reset();
        left_->beginTuple();
        load_matches_for_left();
        if (!advance_to_match()) {
            isend_ = true;
        }
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        has_current_ = false;
        if (!advance_to_match()) {
            isend_ = true;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!has_current_) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        materialize_tuple_view(current_view_, cols_, rec.get(), len_);
        return rec;
    }
    const TupleView *CurrentTupleView() const override { return has_current_ ? &current_view_ : nullptr; }
    const RmRecord *CurrentTuple() const override {
        if (!has_current_) {
            return nullptr;
        }
        materialize_tuple_view(current_view_, cols_, &current_record_, len_);
        return &current_record_;
    }
    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "IndexNestedLoopJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
