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
#include <fstream>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        std::vector<std::unique_ptr<RmRecord>> new_records;
        new_records.reserve(rids_.size());
        for (auto &rid : rids_) {
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr &&
                !context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd())) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            auto rec = fh_->get_record(rid, context_);
            auto new_rec = std::make_unique<RmRecord>(*rec);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (col->type != set_clause.rhs.type) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                }
                if (set_clause.rhs.raw == nullptr) {
                    const_cast<Value &>(set_clause.rhs).init_raw(col->len);
                }
                memcpy(new_rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            new_records.push_back(std::move(new_rec));
        }

        for (size_t rec_idx = 0; rec_idx < new_records.size(); ++rec_idx) {
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::unique_ptr<char[]> key(new char[index.col_tot_len]);
                int offset = 0;
                for (int i = 0; i < index.col_num; ++i) {
                    memcpy(key.get() + offset, new_records[rec_idx]->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                std::vector<Rid> result;
                if (ih->get_value(key.get(), &result, context_->txn_)) {
                    for (auto &existing : result) {
                        if (existing != rids_[rec_idx]) {
                            throw RMDBError("Duplicate key");
                        }
                    }
                }
            }
        }

        for (size_t rec_idx = 0; rec_idx < rids_.size(); ++rec_idx) {
            auto old_rec = fh_->get_record(rids_[rec_idx], context_);
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rids_[rec_idx], *old_rec));
                context_->txn_->upsert_snapshot_record(tab_name_, rids_[rec_idx], *new_records[rec_idx]);
            }
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::unique_ptr<char[]> old_key(new char[index.col_tot_len]);
                std::unique_ptr<char[]> new_key(new char[index.col_tot_len]);
                int offset = 0;
                for (int i = 0; i < index.col_num; ++i) {
                    memcpy(old_key.get() + offset, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    memcpy(new_key.get() + offset, new_records[rec_idx]->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->delete_entry(old_key.get(), context_->txn_);
                ih->insert_entry(new_key.get(), rids_[rec_idx], context_->txn_);
            }
            fh_->update_record(rids_[rec_idx], new_records[rec_idx]->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
