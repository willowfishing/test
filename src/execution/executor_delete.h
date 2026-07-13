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
#include "recovery/log_manager.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid : rids_) {
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr &&
                !context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd())) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }

            // Try to read the record. If it was deleted by another committed txn,
            // this is a write-write conflict for SI/SER isolation.
            std::unique_ptr<RmRecord> rec;
            try {
                rec = fh_->get_record(rid, context_);
            } catch (RMDBError &e) {
                if (context_ != nullptr && context_->txn_ != nullptr &&
                    (context_->txn_->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
                     context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE)) {
                    throw TransactionAbortException(
                        context_->txn_->get_transaction_id(),
                        AbortReason::WRITE_CONFLICT);
                }
                throw;
            }

            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec));
                context_->txn_->remove_snapshot_record(tab_name_, rid);

                // SSI: check rw-dependency for SERIALIZABLE mode
                if (context_->txn_mgr_ != nullptr &&
                    context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    context_->txn_mgr_->CheckSSIRWDependency(context_->txn_, tab_name_, rid);
                }
            }

            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::unique_ptr<char[]> key(new char[index.col_tot_len]);
                int offset = 0;
                for (int i = 0; i < index.col_num; ++i) {
                    memcpy(key.get() + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->delete_entry(key.get(), context_->txn_);
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
