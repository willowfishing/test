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

#include <chrono>

#include "execution_defs.h"
#include "execution_common.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    std::vector<ColMeta> full_cols_;
    std::vector<ColMeta> output_source_cols_;
    size_t len_;                        // scan后生成的每条记录的长度
    size_t full_len_ = 0;
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同
    std::vector<CompiledCondition> compiled_full_conds_;
    std::vector<CompiledCondition> compiled_output_conds_;
    bool compiled_full_conds_valid_ = false;
    bool compiled_output_conds_valid_ = false;

    Rid rid_;
    std::unique_ptr<RmScan> scan_;      // table_iterator
    mutable RmRecord current_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;
    ReadCommittedIndexEntryCursor rc_index_entry_;
    bool at_end_ = true;
    bool early_lock_point_read_ = false;
    BufferAccessClass scan_access_class_ = BufferAccessClass::Default;
    bool track_rmw_point_read_ = false;

    SmManager *sm_manager_;
    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback_;
    std::chrono::steady_clock::time_point feedback_start_;
    uint64_t feedback_rows_scanned_ = 0;
    uint64_t feedback_rows_visible_ = 0;
    uint64_t feedback_rows_output_ = 0;
    bool feedback_started_ = false;
    bool feedback_flushed_ = false;

    void reset_feedback_counters() {
        feedback_rows_scanned_ = 0;
        feedback_rows_visible_ = 0;
        feedback_rows_output_ = 0;
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
                                   feedback_rows_output_, 0, feedback_rows_visible_, elapsed_ns);
    }

    bool eval_current_record_conds() const {
        if (compiled_output_conds_valid_) {
            return eval_compiled_conds_record(&current_record_, compiled_output_conds_);
        }
        return eval_conds(cols_, &current_record_, fed_conds_);
    }

    bool eval_slot_conds(const char *slot) const {
        RmRecord record_view;
        record_view.data = const_cast<char *>(slot);
        record_view.size = static_cast<int>(full_len_);
        record_view.allocated_ = false;
        if (compiled_full_conds_valid_) {
            return eval_compiled_conds_record(&record_view, compiled_full_conds_);
        }
        return eval_conds(full_cols_, &record_view, fed_conds_);
    }

    bool read_current_slot_if_matches(bool *matches) {
        *matches = false;
        return scan_->with_current_slot([&](const char *slot) {
            if (!eval_slot_conds(slot)) {
                return true;
            }
            current_record_.ResizeAndCopy(slot, static_cast<int>(full_len_));
            *matches = true;
            return true;
        });
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

    bool lock_and_refresh_current_rc_record_if_needed() {
        if (!early_lock_point_read_) {
            return true;
        }
        LockRecordForEarlyReadOrAbort(context_, rid_, fh_->GetFd());
        if (!context_->txn_mgr_->GetReadCommittedTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
            return false;
        }
        return eval_conds(full_cols_, &current_record_, fed_conds_);
    }

    void advance_to_next_visible() {
        at_end_ = true;
        while (scan_ != nullptr && !scan_->is_end()) {
            feedback_rows_scanned_++;
            rid_ = scan_->rid();
            if (use_mvcc_visibility()) {
                if (context_->txn_mgr_->GetVisibleTupleInto(tab_name_, rid_, context_->txn_, &current_record_)) {
                    feedback_rows_visible_++;
                    if (!eval_current_record_conds()) {
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
            } else {
                bool matches = false;
                if (!read_current_slot_if_matches(&matches)) {
                    scan_->next();
                    continue;
                }
                auto state = rc_index_entry_.classify(rid_);
                if (state == TransactionManager::ReadCommittedIndexEntryState::INVISIBLE) {
                    scan_->next();
                    continue;
                }
                if (state == TransactionManager::ReadCommittedIndexEntryState::NEEDS_HEAP) {
                    TupleMeta visible_meta{};
                    if (!context_->txn_mgr_->GetReadCommittedTupleInto(
                            tab_name_, rid_, context_->txn_, &current_record_, &visible_meta, nullptr,
                            rc_index_entry_.last_tuple_hint())) {
                        scan_->next();
                        continue;
                    }
                    matches = eval_conds(full_cols_, &current_record_, fed_conds_);
                }
                feedback_rows_visible_++;
                if (!matches) {
                    scan_->next();
                    continue;
                }
                if (!lock_and_refresh_current_rc_record_if_needed()) {
                    scan_->next();
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
            scan_->next();
        }
        flush_feedback();
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context,
                    std::string visible_name = "", std::vector<TabCol> required_cols = {},
                    std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr,
                    std::shared_ptr<rmdb::RuntimeNodeFeedback> runtime_feedback = nullptr) {
        sm_manager_ = sm_manager;
        runtime_feedback_ = std::move(runtime_feedback);
        tab_name_ = std::move(tab_name);
        std::string visible = visible_name.empty() ? tab_name_ : std::move(visible_name);
        conds_ = std::move(conds);
        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        const TabMeta &tab = use_cache ? *runtime_cache->tab : sm_manager_->db_.get_table(tab_name_);
        fh_ = use_cache ? runtime_cache->fh : sm_manager_->fhs_.at(tab_name_).get();
        context_ = context;
        rc_index_entry_.bind(context == nullptr ? nullptr : context->txn_mgr_,
                             context == nullptr ? nullptr : context->txn_, tab_name_);
        if (use_cache) {
            full_cols_ = runtime_cache->full_cols;
            full_len_ = runtime_cache->full_len;
        } else {
            full_cols_ = tab.cols;
            for (auto &col : full_cols_) {
                col.tab_name = visible;
            }
            full_len_ = full_cols_.back().offset + full_cols_.back().len;
        }
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

        fed_conds_ = conds_;
        track_rmw_point_read_ = ShouldTrackReadModifyWritePointRead(context_, visible, required_cols, fed_conds_);
        early_lock_point_read_ = ShouldEarlyLockPointRead(context_, visible, required_cols, fed_conds_, full_cols_);
        compiled_full_conds_valid_ = compile_conds(full_cols_, fed_conds_, &compiled_full_conds_);
        compiled_output_conds_valid_ = compile_conds(cols_, fed_conds_, &compiled_output_conds_);
        scan_access_class_ = fh_->get_file_hdr().num_pages >= 64 ? BufferAccessClass::BulkRead
                                                                  : BufferAccessClass::Default;
    }

    ~SeqScanExecutor() override { flush_feedback(); }

    void beginTuple() override {
        flush_feedback();
        reset_feedback_counters();
        rc_index_entry_.reset();
        current_view_.record = nullptr;
        current_view_.cells = nullptr;
        TransactionManager::record_serializable_predicate_read(context_ == nullptr ? nullptr : context_->txn_,
                                                               tab_name_, full_cols_, fed_conds_);
        scan_ = std::make_unique<RmScan>(fh_, scan_access_class_);
        advance_to_next_visible();
    }

    void nextTuple() override {
        if (scan_ == nullptr || at_end_) {
            return;
        }
        scan_->next();
        advance_to_next_visible();
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
    std::string getType() override { return "SeqScanExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return rid_; }
};
