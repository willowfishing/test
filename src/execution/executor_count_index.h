#pragma once

#include <chrono>
#include <cstring>
#include <unordered_set>

#include "common/index_runtime.h"
#include "common/snapshot_index_history.h"
#include "execution_common.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class CountIndexAggregateExecutor : public AbstractExecutor {
   private:
    struct CompiledKeyCond {
        int lhs_offset = 0;
        int rhs_offset = 0;
        int len = 0;
        EvalCondFn eval_fn = nullptr;
        const char *rhs_value = nullptr;
        bool rhs_is_value = false;
    };

    SmManager *sm_manager_ = nullptr;
    std::string tab_name_;
    std::string visible_name_;
    const TabMeta *tab_ = nullptr;
    std::vector<Condition> conds_;
    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    RmFileHandle *fh_ = nullptr;
    IxIndexHandle *ih_ = nullptr;
    std::vector<ColMeta> index_key_cols_;
    std::vector<CompiledKeyCond> compiled_key_conds_;
    std::vector<ColMeta> full_cols_;
    std::vector<ColMeta> cols_;
    size_t len_ = sizeof(int);
    rmdb::IndexRangeSpec range_spec_;
    std::string visible_key_scratch_;

    ReadCommittedIndexEntryCursor rc_index_entry_;
    RmRecordPageCursor heap_page_cursor_;
    RmRecord current_record_;
    RmRecord output_tuple_;
    size_t cursor_ = 0;
    bool has_tuple_ = false;
    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback_;
    std::chrono::steady_clock::time_point feedback_start_;
    uint64_t feedback_rows_output_ = 0;
    uint64_t feedback_index_entries_ = 0;
    uint64_t feedback_heap_fetches_ = 0;
    bool feedback_started_ = false;
    bool feedback_flushed_ = false;
    bool compiled_key_conds_valid_ = false;

    void reset_feedback_counters() {
        feedback_rows_output_ = 0;
        feedback_index_entries_ = 0;
        feedback_heap_fetches_ = 0;
        feedback_started_ = runtime_feedback_ != nullptr;
        feedback_flushed_ = false;
        if (feedback_started_) {
            feedback_start_ = std::chrono::steady_clock::now();
        }
    }

    void flush_feedback() {
        if (!feedback_started_ || feedback_flushed_) {
            return;
        }
        feedback_flushed_ = true;
        auto elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - feedback_start_)
                .count());
        rmdb::record_scan_feedback(runtime_feedback_, 0, feedback_rows_output_, feedback_rows_output_,
                                   feedback_index_entries_, feedback_heap_fetches_, elapsed_ns);
    }

    bool use_mvcc_visibility() const {
        return context_ != nullptr && UseMvccReadVisibility(context_->txn_);
    }

    Iid lookup_bound(rmdb::IndexBoundLookup lookup, const std::string &key) const {
        switch (lookup) {
            case rmdb::IndexBoundLookup::LeafBegin: return ih_->leaf_begin();
            case rmdb::IndexBoundLookup::LowerBound: return ih_->lower_bound(key.data());
            case rmdb::IndexBoundLookup::UpperBound: return ih_->upper_bound(key.data());
            case rmdb::IndexBoundLookup::LeafEnd: return ih_->leaf_end();
        }
        return ih_->leaf_end();
    }

    void compile_index_key_cols() {
        index_key_cols_.clear();
        int key_offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            ColMeta col = index_col;
            col.tab_name = visible_name_;
            col.offset = key_offset;
            index_key_cols_.push_back(col);
            key_offset += index_col.len;
        }
    }

    void compile_key_conds() {
        compiled_key_conds_.clear();
        compiled_key_conds_.reserve(conds_.size());
        compiled_key_conds_valid_ = true;
        for (const auto &cond : conds_) {
            auto lhs_col = get_col(index_key_cols_, cond.lhs_col);
            CompiledKeyCond compiled;
            compiled.lhs_offset = lhs_col->offset;
            compiled.len = lhs_col->len;
            compiled.eval_fn = condition_eval_fn(lhs_col->type, cond.op);
            compiled.rhs_is_value = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    compiled_key_conds_.clear();
                    compiled_key_conds_valid_ = false;
                    return;
                }
                compiled.rhs_value = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(index_key_cols_, cond.rhs_col);
                if (rhs_col->type != lhs_col->type || rhs_col->len != lhs_col->len) {
                    compiled_key_conds_.clear();
                    compiled_key_conds_valid_ = false;
                    return;
                }
                compiled.rhs_offset = rhs_col->offset;
            }
            compiled_key_conds_.push_back(compiled);
        }
    }

    bool eval_key_conds(const char *key) const {
        if (compiled_key_conds_valid_) {
            for (const auto &cond : compiled_key_conds_) {
                const char *lhs = key + cond.lhs_offset;
                const char *rhs = cond.rhs_is_value ? cond.rhs_value : key + cond.rhs_offset;
                if (!cond.eval_fn(lhs, rhs, cond.len)) {
                    return false;
                }
            }
            return true;
        }
        RmRecord key_record;
        key_record.data = const_cast<char *>(key);
        key_record.size = index_meta_.logical_col_tot_len();
        key_record.allocated_ = false;
        return eval_conds(index_key_cols_, &key_record, conds_);
    }

    bool key_conds_match(const char *key) const {
        return range_spec_.all_conditions_consumed || eval_key_conds(key);
    }

    bool visible_record_matches_index_key(const Rid &rid, const char *key) {
        if (key == nullptr || current_record_.data == nullptr) {
            return false;
        }
        char *visible_key = rmdb::build_index_key_into(index_meta_, current_record_.data, rid, &visible_key_scratch_);
        return std::memcmp(visible_key, key, index_meta_.col_tot_len) == 0;
    }

    bool entry_counts(const Rid &rid, const char *key) {
        auto state = rc_index_entry_.classify(rid);
        if (state == TransactionManager::ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE) {
            return key_conds_match(key);
        }
        if (state == TransactionManager::ReadCommittedIndexEntryState::INVISIBLE) {
            return false;
        }
        feedback_heap_fetches_++;
        bool found = use_mvcc_visibility()
                         ? context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid, context_->txn_, &current_record_)
                         : context_->txn_mgr_->GetReadCommittedTupleInto(
                               tab_name_, rid, context_->txn_, &current_record_, nullptr, &heap_page_cursor_,
                               rc_index_entry_.last_tuple_hint());
        if (!found) {
            return false;
        }
        if (!visible_record_matches_index_key(rid, key)) {
            return false;
        }
        return key_conds_match(key);
    }

    bool scan_prefix_matches(const char *key) const {
        return range_spec_.scan_prefix_len <= 0 ||
               std::memcmp(key, range_spec_.lower_key.data(), range_spec_.scan_prefix_len) == 0;
    }

    static uint64_t rid_key(const Rid &rid) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(rid.page_no)) << 32) |
               static_cast<uint32_t>(rid.slot_no);
    }

    int count_visible_entries() {
        int count = 0;
        std::vector<Rid> historical;
        std::unordered_set<uint64_t> historical_keys;
        if (use_mvcc_visibility()) {
            auto candidates = rmdb::lookup_snapshot_index_history(tab_name_, index_col_names_,
                                                                  context_->txn_->get_read_ts());
            historical.reserve(candidates.size());
            historical_keys.reserve(candidates.size());
            for (const auto &rid : candidates) {
                if (historical_keys.insert(rid_key(rid)).second) {
                    historical.push_back(rid);
                }
            }
        }
        IxReadGuard read_guard = ih_->make_read_guard();
        Iid lower = lookup_bound(range_spec_.lower_lookup, range_spec_.lower_key);
        Iid upper = lookup_bound(range_spec_.upper_lookup, range_spec_.upper_key);
        auto scan = ih_->create_scan(lower, upper);
        page_id_t clean_page_no = RM_NO_PAGE;
        while (scan != nullptr && !scan->is_end()) {
            int slot = scan->current_leaf_slot();
            const int leaf_end = scan->current_leaf_scan_end_slot();
            int processed = 0;
            while (slot < leaf_end) {
                const char *key = scan->current_leaf_key_at(slot);
                if (!scan_prefix_matches(key)) {
                    return count;
                }
                feedback_index_entries_++;
                const Rid rid = *scan->current_leaf_rid_at(slot);
                processed++;

                if (historical_keys.find(rid_key(rid)) != historical_keys.end()) {
                    clean_page_no = RM_NO_PAGE;
                    slot++;
                    continue;
                }

                if (rid.page_no == clean_page_no) {
                    bool clean_visible = rc_index_entry_.current_page_still_clean_visible(rid.page_no);
                    if (clean_visible && range_spec_.all_conditions_consumed) {
                        count++;
                        slot++;
                        while (slot < leaf_end) {
                            const char *next_key = scan->current_leaf_key_at(slot);
                            if (!scan_prefix_matches(next_key)) {
                                scan->advance_current_leaf(processed);
                                return count;
                            }
                            const Rid next_rid = *scan->current_leaf_rid_at(slot);
                            if (next_rid.page_no != clean_page_no) {
                                break;
                            }
                            feedback_index_entries_++;
                            count++;
                            processed++;
                            slot++;
                        }
                        continue;
                    }
                    if (clean_visible && key_conds_match(key)) {
                        count++;
                    }
                    slot++;
                    continue;
                }

                if (entry_counts(rid, key)) {
                    count++;
                }
                clean_page_no = rc_index_entry_.current_page_clean_visible() ? rid.page_no : RM_NO_PAGE;
                slot++;
            }
            scan->advance_current_leaf(processed);
        }
        read_guard.reset();
        for (const auto &rid : historical) {
            feedback_heap_fetches_++;
            if (context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid, context_->txn_, &current_record_) &&
                eval_conds(full_cols_, &current_record_, conds_)) {
                count++;
            }
        }
        return count;
    }

    void write_count(int value) {
        output_tuple_ = RmRecord(static_cast<int>(len_));
        std::memcpy(output_tuple_.data, &value, sizeof(int));
    }

   public:
    CountIndexAggregateExecutor(SmManager *sm_manager,
                                std::string tab_name,
                                std::string visible_name,
                                std::vector<Condition> conds,
                                std::vector<std::string> index_col_names,
                                std::vector<TabCol> output_cols,
                                Context *context,
                                std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                                std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        context_ = context;
        runtime_feedback_ = std::move(runtime_feedback);
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        conds_ = std::move(conds);
        index_col_names_ = std::move(index_col_names);

        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = use_cache ? runtime_cache->tab : &sm_manager_->db_.get_table(tab_name_);
        fh_ = use_cache && runtime_cache->fh != nullptr ? runtime_cache->fh : sm_manager_->fhs_.at(tab_name_).get();
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
        } else {
            full_cols_ = tab_->cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible_name_;
            }
        }
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(tab_->get_index_meta(index_col_names_, true));
        ih_ = runtime_cache != nullptr && runtime_cache->has_index
                  ? runtime_cache->ih
                  : rmdb::resolve_index_handle(sm_manager_, tab_name_, index_meta_);
        compile_index_key_cols();
        compile_key_conds();
        range_spec_ = rmdb::build_index_range_spec(index_meta_, conds_, visible_name_);
        visible_key_scratch_.resize(index_meta_.col_tot_len);
        rc_index_entry_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                             context_ == nullptr ? nullptr : context_->txn_, tab_name_);
        heap_page_cursor_.bind(fh_);

        ColMeta output_col;
        output_col.tab_name = "";
        output_col.name = output_cols.empty() ? "count" : output_cols[0].col_name;
        output_col.offset = 0;
        output_col.type = TYPE_INT;
        output_col.len = sizeof(int);
        cols_.push_back(output_col);
    }

    ~CountIndexAggregateExecutor() override { flush_feedback(); }

    void beginTuple() override {
        flush_feedback();
        reset_feedback_counters();
        cursor_ = 0;
        has_tuple_ = true;
        rc_index_entry_.reset();
        heap_page_cursor_.reset();
        int count = count_visible_entries();
        heap_page_cursor_.reset();
        feedback_rows_output_ = static_cast<uint64_t>(count);
        write_count(count);
        flush_feedback();
    }

    void nextTuple() override {
        if (cursor_ == 0) {
            cursor_ = 1;
        }
    }

    bool is_end() const override { return !has_tuple_ || cursor_ > 0; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "CountIndexAggregateExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        std::memcpy(rec->data, output_tuple_.data, len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        return is_end() ? nullptr : &output_tuple_;
    }
};
