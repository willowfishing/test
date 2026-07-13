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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator
    std::unique_ptr<RmRecord> cached_rec_;  // cached visible version

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    static int compare_value(ColType lhs_type, ColType rhs_type, const char *lhs, const char *rhs, int lhs_len = 0, int rhs_len = 0) {
        if (lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) {
            float l = *reinterpret_cast<const float *>(lhs);
            float r = static_cast<float>(*reinterpret_cast<const int *>(rhs));
            return (l > r) ? 1 : ((l < r) ? -1 : 0);
        } else if (lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) {
            float l = static_cast<float>(*reinterpret_cast<const int *>(lhs));
            float r = *reinterpret_cast<const float *>(rhs);
            return (l > r) ? 1 : ((l < r) ? -1 : 0);
        } else if (lhs_type == TYPE_INT) {
            int l = *reinterpret_cast<const int *>(lhs);
            int r = *reinterpret_cast<const int *>(rhs);
            return (l > r) ? 1 : ((l < r) ? -1 : 0);
        } else if (lhs_type == TYPE_FLOAT) {
            float l = *reinterpret_cast<const float *>(lhs);
            float r = *reinterpret_cast<const float *>(rhs);
            return (l > r) ? 1 : ((l < r) ? -1 : 0);
        } else if (lhs_type == TYPE_STRING) {
            // Use column length for safer comparison (avoid over-read)
            int cmp_len = lhs_len > 0 ? lhs_len : 256;
            return strncmp(lhs, rhs, cmp_len);
        }
        return 0;
    }

    static bool check_cmp(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

    bool check_condition(const RmRecord &rec) {
        for (auto &cond : fed_conds_) {
            auto lhs_pos = get_col(cols_, cond.lhs_col);
            char *lhs_buf = rec.data + lhs_pos->offset;
            if (cond.is_rhs_val) {
                // 列 op 值
                int cmp = compare_value(lhs_pos->type, cond.rhs_val.type, lhs_buf, cond.rhs_val.raw->data,
                                        lhs_pos->len, lhs_pos->len);
                if (!check_cmp(cmp, cond.op)) return false;
            } else {
                // 列 op 列
                auto rhs_pos = get_col(cols_, cond.rhs_col);
                char *rhs_buf = rec.data + rhs_pos->offset;
                int cmp = compare_value(lhs_pos->type, rhs_pos->type, lhs_buf, rhs_buf,
                                        lhs_pos->len, rhs_pos->len);
                if (!check_cmp(cmp, cond.op)) return false;
            }
        }
        return true;
    }

    std::unique_ptr<RmRecord> get_visible_rec(TupleMeta *out_meta = nullptr) {
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr)
            return fh_->get_record(rid_, context_);
        auto [slot_meta, slot_rec] = fh_->get_meta_and_record(rid_);
        if (out_meta) *out_meta = slot_meta;
        if (slot_meta.writer_txn_ == context_->txn_->get_transaction_id() ||
            context_->txn_mgr_->IsTupleVisible(slot_meta, context_->txn_))
            return std::move(slot_rec);
        return context_->txn_mgr_->GetVisibleVersion(rid_, fh_, context_->txn_);
    }

    void record_ssi_read(const TupleMeta &slot_meta) {
        if (context_ && context_->txn_ && context_->txn_mgr_ &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            context_->txn_mgr_->AddToGlobalReadSet(rid_, context_->txn_->get_transaction_id());
            context_->txn_->add_to_read_set(rid_);
            // Check invisible writes: if slot has a version not visible to me
            // (committed after my snapshot or in-progress), record rw-in
            if (slot_meta.writer_txn_ != INVALID_TXN_ID &&
                slot_meta.writer_txn_ != context_->txn_->get_transaction_id() &&
                !context_->txn_mgr_->IsTupleVisible(slot_meta, context_->txn_)) {
                auto *w = context_->txn_mgr_->get_transaction_safe(slot_meta.writer_txn_);
                if (w && w->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    context_->txn_->add_rw_in(slot_meta.writer_txn_);
                    w->add_rw_out(context_->txn_->get_transaction_id());
                }
            }
        }
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        cached_rec_.reset();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            TupleMeta slot_meta;
            cached_rec_ = get_visible_rec(&slot_meta);
            if (cached_rec_ && check_condition(*cached_rec_)) { record_ssi_read(slot_meta); return; }
            scan_->next();
        }
        // Predicate read for phantom detection: record when scan had WHERE conditions
        // (even if results were non-empty, concurrent INSERTs could create phantoms)
        if (context_ && context_->txn_ && context_->txn_mgr_ &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE &&
            !fed_conds_.empty()) {
            context_->txn_->add_predicate_read(tab_name_);
        }
    }

    void nextTuple() override {
        scan_->next();
        cached_rec_.reset();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            TupleMeta slot_meta;
            cached_rec_ = get_visible_rec(&slot_meta);
            if (cached_rec_ && check_condition(*cached_rec_)) { record_ssi_read(slot_meta); return; }
            scan_->next();
        }
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    bool is_end() const override {
        return scan_ == nullptr || scan_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (cached_rec_) {
            auto rec = std::make_unique<RmRecord>(cached_rec_->size);
            memcpy(rec->data, cached_rec_->data, cached_rec_->size);
            return rec;
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }
};