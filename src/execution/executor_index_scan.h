/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_set>
#include <utility>

#include "common/index_runtime.h"
#include "common/snapshot_index_history.h"
#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_scan_cache.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    struct CompiledSlotCond {
        int lhs_offset;
        int rhs_offset;
        int len;
        ColType type;
        CompOp op;
        EvalCondFn eval_fn;
        const char *rhs_value;
        bool rhs_is_value;
    };
    struct PrefixCond {
        int key_offset;
        int len;
        const char *rhs_value;
    };

    static CompOp swap_comparison_op(CompOp op) {
        switch (op) {
            case OP_EQ: return OP_EQ;
            case OP_NE: return OP_NE;
            case OP_LT: return OP_GT;
            case OP_GT: return OP_LT;
            case OP_LE: return OP_GE;
            case OP_GE: return OP_LE;
        }
        return op;
    }

    std::string tab_name_;                      // 表名称
    std::string visible_name_;
    const TabMeta *tab_ = nullptr;              // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    std::vector<ColMeta> full_cols_;
    std::vector<ColMeta> output_source_cols_;
    std::vector<ColMeta> index_key_cols_;
    std::vector<ColMeta> covering_source_cols_;
    size_t len_;                                // 选取出来的一条记录的长度
    size_t full_len_ = 0;
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据
    IxIndexHandle *ih_{nullptr};

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::vector<Rid> matched_rids_;
    size_t cursor_ = 0;
    mutable RmRecord current_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;
    std::unique_ptr<IxScan> index_scan_;
    IxReadGuard index_read_guard_;
    int scan_prefix_len_ = 0;
    bool at_end_ = true;
    bool fallback_seq_scan_ = false;
    bool materialized_exact_key_ = false;
    bool compiled_slot_conds_valid_ = false;
    std::vector<CompiledSlotCond> compiled_slot_conds_;
    std::vector<PrefixCond> compiled_prefix_conds_;
    int compiled_equality_prefix_cols_ = 0;
    int compiled_equality_prefix_len_ = 0;
    std::string index_key_scratch_;
    std::string visible_key_scratch_;
    rmdb::IndexRangeSpec range_spec_;
    std::vector<std::string> cache_epoch_columns_;
    bool covering_mode_ = false;
    bool early_lock_point_read_ = false;
    bool track_rmw_point_read_ = false;

    // Cached data page for amortizing BPM fetch_page/unpin_page across consecutive RIDs
    RmRecordPageCursor data_page_cursor_;
    ReadCommittedIndexEntryCursor rc_index_entry_;

    SmManager *sm_manager_;
    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback_;
    std::chrono::steady_clock::time_point feedback_start_;
    uint64_t feedback_rows_scanned_ = 0;
    uint64_t feedback_rows_visible_ = 0;
    uint64_t feedback_rows_output_ = 0;
    uint64_t feedback_index_entries_ = 0;
    uint64_t feedback_heap_fetches_ = 0;
    bool feedback_started_ = false;
    bool feedback_flushed_ = false;

    void reset_feedback_counters() {
        feedback_rows_scanned_ = 0;
        feedback_rows_visible_ = 0;
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
        rmdb::record_scan_feedback(runtime_feedback_, feedback_rows_scanned_, feedback_rows_visible_,
                                   feedback_rows_output_, feedback_index_entries_, feedback_heap_fetches_,
                                   elapsed_ns);
    }
    
    void reset_data_page() {
        data_page_cursor_.reset();
    }
    
    const char *get_record_slot(const Rid &rid) {
        return data_page_cursor_.get_slot(rid);
    }

    void record_current_read() {
        TransactionManager::record_serializable_read(context_ == nullptr ? nullptr : context_->txn_, tab_name_, rid_);
        if (!track_rmw_point_read_ || !IsExplicitReadCommittedTxn(context_) || !current_view_) {
            return;
        }
        for (size_t i = 0; i < cols_.size(); ++i) {
            const auto &col = cols_[i];
            if (col.type != TYPE_INT && col.type != TYPE_FLOAT) {
                continue;
            }
            context_->txn_->remember_read_column(tab_name_, rid_, col, current_view_.cell_at(col, i));
        }
    }

    bool use_mvcc_visibility() const {
        return context_ != nullptr && context_->txn_mgr_ != nullptr && UseMvccReadVisibility(context_->txn_);
    }

    bool need_snapshot_fallback_seq() const {
        return false;
    }

    void materialize_snapshot_index_candidates() {
        std::unordered_set<uint64_t> seen;
        auto rid_key = [](const Rid &rid) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(rid.page_no)) << 32) |
                   static_cast<uint32_t>(rid.slot_no);
        };
        std::vector<Rid> candidates;
        candidates.reserve(matched_rids_.size());
        for (const auto &rid : matched_rids_) {
            if (seen.insert(rid_key(rid)).second) {
                candidates.push_back(rid);
            }
        }
        while (index_scan_ != nullptr && !index_scan_->is_end()) {
            if (scan_prefix_len_ > 0 &&
                std::memcmp(index_scan_->key(), index_key_scratch_.data(), scan_prefix_len_) != 0) {
                break;
            }
            Rid rid = index_scan_->rid();
            if (seen.insert(rid_key(rid)).second) {
                candidates.push_back(rid);
            }
            index_scan_->next();
        }
        index_scan_.reset();
        index_read_guard_.reset();

        auto historical = rmdb::lookup_snapshot_index_history(tab_name_, index_col_names_,
                                                              context_->txn_->get_read_ts());
        for (const auto &rid : historical) {
            if (seen.insert(rid_key(rid)).second) {
                candidates.push_back(rid);
            }
        }
        matched_rids_ = std::move(candidates);
        cursor_ = 0;
    }

    bool visible_record_matches_index_key(const char *key) {
        if (key == nullptr || current_record_.data == nullptr) {
            return false;
        }
        char *visible_key = rmdb::build_index_key_into(index_meta_, current_record_.data, rid_, &visible_key_scratch_);
        return memcmp(visible_key, key, index_meta_.col_tot_len) == 0;
    }

    bool lock_and_refresh_current_rc_record_if_needed(const char *key) {
        if (!early_lock_point_read_) {
            return true;
        }
        LockRecordForEarlyReadOrAbort(context_, rid_, fh_->GetFd());
        feedback_heap_fetches_++;
        if (!context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
            return false;
        }
        if (key != nullptr && !visible_record_matches_index_key(key)) {
            return false;
        }
        return eval_conds(full_cols_, &current_record_, fed_conds_);
    }

    void set_current_from_covering_key(const char *key) {
        current_cells_.resize(covering_source_cols_.size());
        for (size_t i = 0; i < covering_source_cols_.size(); ++i) {
            current_cells_[i] = key + covering_source_cols_[i].offset;
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
    }

    bool try_covering_fast_path(const char *key) {
        if (!covering_mode_) {
            return false;
        }
        auto state = rc_index_entry_.classify(rid_);
        if (state == TransactionManager::ReadCommittedIndexEntryState::INVISIBLE) {
            return false;
        }
        if (state == TransactionManager::ReadCommittedIndexEntryState::NEEDS_HEAP) {
            feedback_heap_fetches_++;
            if (!context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
                return false;
            }
            if (!visible_record_matches_index_key(key)) {
                return false;
            }
        }
        feedback_rows_visible_++;
        if (!eval_key_conds(key)) {
            return false;
        }
        set_current_from_covering_key(key);
        return true;
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

    bool is_prefix_index_col(const std::string &col_name, int equality_prefix_cols) const {
        for (int i = 0; i < equality_prefix_cols && i < index_meta_.col_num; ++i) {
            if (index_meta_.cols[i].name == col_name) {
                return true;
            }
        }
        return false;
    }

    bool can_use_filtered_rid_cache(int equality_prefix_cols) const {
        if (use_mvcc_visibility() || equality_prefix_cols <= 0 || equality_prefix_cols >= index_meta_.col_num) {
            return false;
        }
        if (context_ != nullptr && context_->txn_ != nullptr && context_->txn_->get_txn_mode()) {
            return false;
        }
        bool has_residual_equality = false;
        for (const auto &cond : fed_conds_) {
            if (!cond.is_rhs_val || cond.op != OP_EQ || cond.rhs_val.raw == nullptr ||
                cond.lhs_col.tab_name != visible_name_) {
                return false;
            }
            if (!is_prefix_index_col(cond.lhs_col.col_name, equality_prefix_cols)) {
                has_residual_equality = true;
            }
        }
        return has_residual_equality;
    }

    std::vector<std::string> cache_epoch_columns() const {
        if (!cache_epoch_columns_.empty()) {
            return cache_epoch_columns_;
        }
        std::set<std::string> unique_cols;
        for (const auto &cond : fed_conds_) {
            unique_cols.insert(cond.lhs_col.col_name);
        }
        return {unique_cols.begin(), unique_cols.end()};
    }

    std::string filtered_cache_key() const {
        std::vector<std::string> parts;
        parts.reserve(fed_conds_.size());
        for (const auto &cond : fed_conds_) {
            auto col_it = get_col(full_cols_, cond.lhs_col);
            if (col_it == full_cols_.end()) {
                throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
            }
            const auto &col = *col_it;
            std::string part = col.name;
            part.push_back('\x1f');
            part.push_back(static_cast<char>('0' + static_cast<int>(cond.op)));
            part.push_back('\x1f');
            part += std::to_string(col.len);
            part.push_back('\x1f');
            part.append(cond.rhs_val.raw->data, col.len);
            parts.push_back(std::move(part));
        }
        std::sort(parts.begin(), parts.end());

        std::string key = tab_name_;
        key.push_back('\x1d');
        for (const auto &part : parts) {
            key += part;
            key.push_back('\x1e');
        }
        return key;
    }

    bool eval_compiled_slot_conds(const char *slot) const {
        for (const auto &cond : compiled_slot_conds_) {
            const char *lhs = slot + cond.lhs_offset;
            const char *rhs = cond.rhs_is_value ? cond.rhs_value : slot + cond.rhs_offset;
            if (!cond.eval_fn(lhs, rhs, cond.len)) {
                return false;
            }
        }
        return true;
    }

    bool eval_slot_conds(const char *slot) const {
        if (compiled_slot_conds_valid_) {
            return eval_compiled_slot_conds(slot);
        }
        RmRecord record_view;
        record_view.data = const_cast<char *>(slot);
        record_view.size = static_cast<int>(full_len_);
        record_view.allocated_ = false;
        return eval_conds(full_cols_, &record_view, fed_conds_);
    }

    bool eval_key_conds(const char *key) const {
        RmRecord key_record;
        key_record.data = const_cast<char *>(key);
        key_record.size = index_meta_.logical_col_tot_len();
        key_record.allocated_ = false;
        return eval_conds(index_key_cols_, &key_record, fed_conds_);
    }

    void compile_slot_conds() {
        compiled_slot_conds_.clear();
        compiled_slot_conds_.reserve(fed_conds_.size());
        compiled_slot_conds_valid_ = true;
        for (const auto &cond : fed_conds_) {
            auto lhs_col = get_col(full_cols_, cond.lhs_col);
            CompiledSlotCond compiled{lhs_col->offset, 0, lhs_col->len, lhs_col->type, cond.op,
                                      condition_eval_fn(lhs_col->type, cond.op), nullptr, cond.is_rhs_val};
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    compiled_slot_conds_valid_ = false;
                    compiled_slot_conds_.clear();
                    return;
                }
                compiled.rhs_value = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(full_cols_, cond.rhs_col);
                if (rhs_col->type != lhs_col->type || rhs_col->len != lhs_col->len) {
                    compiled_slot_conds_valid_ = false;
                    compiled_slot_conds_.clear();
                    return;
                }
                compiled.rhs_offset = rhs_col->offset;
            }
            compiled_slot_conds_.push_back(compiled);
        }
    }

    void compile_index_prefix() {
        compiled_prefix_conds_.clear();
        compiled_equality_prefix_cols_ = 0;
        compiled_equality_prefix_len_ = 0;
        int key_offset = 0;
        for (const auto &index_col : index_meta_.cols) {
            auto cond_it = std::find_if(fed_conds_.begin(), fed_conds_.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
                       cond.lhs_col.tab_name == visible_name_ && cond.lhs_col.col_name == index_col.name;
            });
            if (cond_it == fed_conds_.end()) {
                break;
            }
            compiled_prefix_conds_.push_back({key_offset, index_col.len, cond_it->rhs_val.raw->data});
            key_offset += index_col.len;
            compiled_equality_prefix_cols_++;
            compiled_equality_prefix_len_ = key_offset;
        }
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

    bool try_enable_covering_mode(const std::vector<TabCol> &required_cols) {
        if (required_cols.empty() || use_mvcc_visibility()) {
            return false;
        }
        if (!rmdb::index_covers_required_and_conditions(index_meta_, required_cols, fed_conds_, visible_name_)) {
            return false;
        }
        build_column_projection(index_key_cols_, required_cols, &cols_, &covering_source_cols_, &len_);
        if (cols_.empty()) {
            return false;
        }
        output_source_cols_ = covering_source_cols_;
        covering_mode_ = true;
        return true;
    }

    void filter_and_cache_matched_rids(const std::string &cache_key, const std::vector<std::string> &epoch_cols) {
        std::vector<Rid> filtered;
        RmRecordPageCursor filter_cursor(fh_);
        ReadCommittedVisibilityCursor filter_visibility(context_ == nullptr ? nullptr : context_->txn_mgr_,
                                                        context_ == nullptr ? nullptr : context_->txn_, tab_name_);
        auto handle_rid = [&](const Rid &rid) {
            if (!filter_visibility.visible(rid)) {
                return;
            }
            bool matched = filter_cursor.with_slot(rid, [&](const char *slot) { return eval_slot_conds(slot); });
            if (matched) {
                filtered.push_back(rid);
            }
        };
        if (index_scan_ != nullptr) {
            while (!index_scan_->is_end()) {
                if (scan_prefix_len_ > 0 &&
                    std::memcmp(index_scan_->key(), index_key_scratch_.data(), scan_prefix_len_) != 0) {
                    break;
                }
                handle_rid(index_scan_->rid());
                index_scan_->next();
            }
            index_scan_.reset();
            index_read_guard_.reset();
        } else {
            filtered.reserve(matched_rids_.size());
            for (const auto &rid : matched_rids_) {
                handle_rid(rid);
            }
        }
        matched_rids_ = std::move(filtered);
        rmdb::store_scan_cache(cache_key, tab_name_, epoch_cols, matched_rids_);
    }

    void advance_materialized_rids() {
        at_end_ = true;
        while (cursor_ < matched_rids_.size()) {
            feedback_index_entries_++;
            rid_ = matched_rids_[cursor_];
            if (use_mvcc_visibility()) {
                feedback_heap_fetches_++;
                if (context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
                    feedback_rows_visible_++;
                    if (!eval_conds(cols_, &current_record_, fed_conds_)) {
                        cursor_++;
                        continue;
                    }
                    feedback_rows_output_++;
                    current_view_.record = &current_record_;
                    current_view_.cells = nullptr;
                    at_end_ = false;
                    record_current_read();
                    return;
                }
            } else {
                const char *key = materialized_exact_key_ ? range_spec_.lower_key.data() : nullptr;
                if (covering_mode_ && key != nullptr) {
                    if (!try_covering_fast_path(key)) {
                        cursor_++;
                        continue;
                    }
                    feedback_rows_output_++;
                    at_end_ = false;
                    record_current_read();
                    return;
                }
                feedback_heap_fetches_++;
                if (!context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid_, context_->txn_,
                                                                    &current_record_)) {
                    cursor_++;
                    continue;
                }
                feedback_rows_visible_++;
                if (materialized_exact_key_ && !visible_record_matches_index_key(range_spec_.lower_key.data())) {
                    cursor_++;
                    continue;
                }
                if (!eval_conds(full_cols_, &current_record_, fed_conds_)) {
                    cursor_++;
                    continue;
                }
                if (!lock_and_refresh_current_rc_record_if_needed(
                        materialized_exact_key_ ? range_spec_.lower_key.data() : nullptr)) {
                    cursor_++;
                    continue;
                }
                feedback_rows_output_++;
                current_cells_.resize(output_source_cols_.size());
                for (size_t i = 0; i < output_source_cols_.size(); ++i) {
                    current_cells_[i] = current_record_.data + output_source_cols_[i].offset;
                }
                current_view_.record = nullptr;
                current_view_.cells = &current_cells_;
                at_end_ = false;
                record_current_read();
                return;
            }
            cursor_++;
        }
        reset_data_page();
        flush_feedback();
    }

    void advance_index_scan_cursor() {
        at_end_ = true;
        while (index_scan_ != nullptr && !index_scan_->is_end()) {
            if (scan_prefix_len_ > 0 &&
                std::memcmp(index_scan_->key(), index_key_scratch_.data(), scan_prefix_len_) != 0) {
                index_scan_.reset();
                index_read_guard_.reset();
                reset_data_page();
                flush_feedback();
                return;
            }
            feedback_index_entries_++;
            rid_ = index_scan_->rid();
            if (covering_mode_) {
                const char *key = index_scan_->key();
                if (!try_covering_fast_path(key)) {
                    index_scan_->next();
                    continue;
                }
                feedback_rows_output_++;
                at_end_ = false;
                record_current_read();
                return;
            }
            feedback_heap_fetches_++;
            if (use_mvcc_visibility()) {
                if (!context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid_, context_->txn_, &current_record_) ||
                    !eval_conds(full_cols_, &current_record_, fed_conds_)) {
                    index_scan_->next();
                    continue;
                }
            } else {
                if (!context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid_, context_->txn_,
                                                                   &current_record_) ||
                    !visible_record_matches_index_key(index_scan_->key()) ||
                    !eval_conds(full_cols_, &current_record_, fed_conds_) ||
                    !lock_and_refresh_current_rc_record_if_needed(index_scan_->key())) {
                    index_scan_->next();
                    continue;
                }
            }
            feedback_rows_visible_++;
            feedback_rows_output_++;
            current_cells_.resize(output_source_cols_.size());
            for (size_t i = 0; i < output_source_cols_.size(); ++i) {
                current_cells_[i] = current_record_.data + output_source_cols_[i].offset;
            }
            current_view_.record = nullptr;
            current_view_.cells = &current_cells_;
            at_end_ = false;
            record_current_read();
            return;
        }
        index_scan_.reset();
        index_read_guard_.reset();
        reset_data_page();
        flush_feedback();
    }

    void advance_index_cursor() {
        if (index_scan_ != nullptr) {
            advance_index_scan_cursor();
            return;
        }
        advance_materialized_rids();
    }

    void advance_seq_fallback() {
        at_end_ = true;
        while (scan_ != nullptr && !scan_->is_end()) {
            feedback_rows_scanned_++;
            rid_ = scan_->rid();
            feedback_heap_fetches_++;
            if (context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
                feedback_rows_visible_++;
                if (!eval_conds(cols_, &current_record_, fed_conds_)) {
                    scan_->next();
                    continue;
                }
                feedback_rows_output_++;
                current_view_.record = &current_record_;
                current_view_.cells = nullptr;
                at_end_ = false;
                record_current_read();
                return;
            }
            scan_->next();
        }
        flush_feedback();
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                    std::vector<std::string> index_col_names, Context *context, std::string visible_name = "",
                    std::vector<TabCol> required_cols = {},
                    std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        context_ = context;
        runtime_feedback_ = std::move(runtime_feedback);
        tab_name_ = std::move(tab_name);
        visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = use_cache ? runtime_cache->tab : &sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = std::move(index_col_names);
        index_meta_ = runtime_cache != nullptr && runtime_cache->has_index
                          ? runtime_cache->index_meta
                          : *(tab_->get_index_meta(index_col_names_, true));
        fh_ = use_cache ? runtime_cache->fh : sm_manager_->fhs_.at(tab_name_).get();
        data_page_cursor_.bind(fh_);
        rc_index_entry_.bind(context_ == nullptr ? nullptr : context_->txn_mgr_,
                             context_ == nullptr ? nullptr : context_->txn_, tab_name_);
        ih_ = runtime_cache != nullptr && runtime_cache->has_index
                  ? runtime_cache->ih
                  : rmdb::resolve_index_handle(sm_manager_, tab_name_, index_meta_);
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
            full_len_ = runtime_cache->full_len;
        } else {
            full_cols_ = tab_->cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible_name_;
            }
            full_len_ = full_cols_.back().offset + full_cols_.back().len;
        }
        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != visible_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == visible_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_comparison_op(cond.op);
            }
        }
        fed_conds_ = conds_;
        compile_index_key_cols();
        track_rmw_point_read_ =
            ShouldTrackReadModifyWritePointRead(context_, visible_name_, required_cols, fed_conds_);
        early_lock_point_read_ =
            ShouldEarlyLockPointRead(context_, visible_name_, required_cols, fed_conds_, full_cols_);
        if (early_lock_point_read_ || !try_enable_covering_mode(required_cols)) {
            if (required_cols.empty() || use_mvcc_visibility()) {
                cols_ = full_cols_;
                output_source_cols_ = full_cols_;
                len_ = full_len_;
            } else {
                build_column_projection(full_cols_, required_cols, &cols_, &output_source_cols_, &len_);
                if (cols_.empty()) {
                    cols_ = full_cols_;
                    output_source_cols_ = full_cols_;
                    len_ = full_len_;
                }
            }
        }
        index_key_scratch_.resize(index_meta_.col_tot_len);
        visible_key_scratch_.resize(index_meta_.col_tot_len);
        cache_epoch_columns_ = cache_epoch_columns();
        compile_slot_conds();
        compile_index_prefix();
        range_spec_ = rmdb::build_index_range_spec(index_meta_, fed_conds_, visible_name_);
    }

    ~IndexScanExecutor() override { flush_feedback(); }

    void beginTuple() override {
        flush_feedback();
        reset_feedback_counters();
        index_scan_.reset();
        index_read_guard_.reset();
        rc_index_entry_.reset();
        current_view_.record = nullptr;
        current_view_.cells = nullptr;
        TransactionManager::record_serializable_predicate_read(context_ == nullptr ? nullptr : context_->txn_,
                                                               tab_name_, full_cols_, fed_conds_);
        fallback_seq_scan_ = need_snapshot_fallback_seq();
        if (fallback_seq_scan_) {
            scan_ = std::make_unique<RmScan>(fh_);
            advance_seq_fallback();
            return;
        }
        matched_rids_.clear();
        cursor_ = 0;
        scan_prefix_len_ = 0;
        materialized_exact_key_ = false;
        int equality_prefix_cols = range_spec_.equality_prefix_cols;
        bool use_filtered_cache = !covering_mode_ && can_use_filtered_rid_cache(equality_prefix_cols);
        std::string cache_key;
        std::vector<std::string> epoch_cols;
        index_read_guard_ = ih_->make_read_guard();
        Iid lower = lookup_bound(range_spec_.lower_lookup, range_spec_.lower_key);
        Iid upper = lookup_bound(range_spec_.upper_lookup, range_spec_.upper_key);
        if (use_filtered_cache) {
            cache_key = filtered_cache_key();
            epoch_cols = cache_epoch_columns_;
            auto cached_rids = rmdb::lookup_scan_cache(cache_key);
            if (cached_rids.has_value()) {
                matched_rids_ = std::move(*cached_rids);
                index_read_guard_.reset();
                advance_index_cursor();
                return;
            }
            index_scan_ = ih_->create_scan(lower, upper);
            scan_prefix_len_ = range_spec_.scan_prefix_len;
            index_key_scratch_ = range_spec_.lower_key;
            filter_and_cache_matched_rids(cache_key, epoch_cols);
            advance_index_cursor();
            return;
        }
        if (range_spec_.exact_unique_key) {
            ih_->get_value(range_spec_.lower_key.data(), &matched_rids_, context_->txn_);
            index_read_guard_.reset();
            materialized_exact_key_ = true;
        } else {
            index_scan_ = ih_->create_scan(lower, upper);
            scan_prefix_len_ = range_spec_.scan_prefix_len;
            index_key_scratch_ = range_spec_.lower_key;
        }
        if (use_mvcc_visibility()) {
            materialize_snapshot_index_candidates();
        }
        advance_index_cursor();
    }

    void nextTuple() override {
        if (fallback_seq_scan_) {
            if (scan_ == nullptr || at_end_) {
                return;
            }
            scan_->next();
            advance_seq_fallback();
            return;
        }
        if (at_end_) {
            return;
        }
        if (index_scan_ != nullptr) {
            index_scan_->next();
        } else if (cursor_ < matched_rids_.size()) {
            cursor_++;
        }
        advance_index_cursor();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (at_end_) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        materialize_tuple_view(*CurrentTupleView(), cols_, rec.get(), len_);
        return rec;
    }

    const RmRecord *CurrentTuple() const override {
        if (at_end_) {
            return nullptr;
        }
        if (current_view_.record == &current_record_) {
            return &current_record_;
        }
        materialize_tuple_view(*CurrentTupleView(), cols_, &current_record_, len_);
        return &current_record_;
    }

    const TupleView *CurrentTupleView() const override {
        if (at_end_) {
            return nullptr;
        }
        if (current_view_.cells != nullptr || current_view_.record != nullptr) {
            return &current_view_;
        }
        return nullptr;
    }

    bool is_end() const override { return at_end_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "IndexScanExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return rid_; }
};
