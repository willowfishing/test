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

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
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
        // Sort rids for consistent lock ordering (deadlock prevention)
        std::sort(rids_.begin(), rids_.end(), [](const Rid &a, const Rid &b) {
            if (a.page_no != b.page_no) return a.page_no < b.page_no;
            return a.slot_no < b.slot_no;
        });
        for (auto &rid : rids_) {
            // Write-write conflict detection via atomic claiming (MVCC)
            if (context_ && context_->txn_ && context_->txn_mgr_) {
                TupleMeta meta = fh_->get_meta(rid);
                if (meta.writer_txn_ != INVALID_TXN_ID &&
                    meta.writer_txn_ != context_->txn_->get_transaction_id()) {
                    auto *writer = context_->txn_mgr_->get_transaction_safe(meta.writer_txn_);
                    if (writer && writer->get_state() == TransactionState::COMMITTED &&
                        writer->get_commit_ts() > context_->txn_->get_start_ts()) {
                        throw TransactionAbortException(context_->txn_->get_transaction_id(),
                            AbortReason::DEADLOCK_PREVENTION);
                    }
                }
                if (!context_->txn_mgr_->TryClaimWrite(rid, context_->txn_->get_transaction_id())) {
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
                // SSI: rw-out dependency
                if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    auto readers = context_->txn_mgr_->GetReaders(rid);
                    for (auto reader_id : readers) {
                        if (reader_id != context_->txn_->get_transaction_id()) {
                            auto *reader = context_->txn_mgr_->get_transaction_safe(reader_id);
                            if (reader && reader->get_state() != TransactionState::COMMITTED &&
                                reader->get_state() != TransactionState::ABORTED) {
                                reader->add_rw_in(context_->txn_->get_transaction_id());
                                context_->txn_->add_rw_out(reader_id);
                            }
                        }
                    }
                }
            }
            // Save old record for rollback and version chain
            auto old_rec = fh_->get_record(rid, context_);
            if (context_ && context_->txn_ && context_->txn_mgr_) {
                TupleMeta old_meta = fh_->get_meta(rid);
                auto old_data = new RmRecord(fh_->get_file_hdr().record_size);
                memcpy(old_data->data, old_rec->data, fh_->get_file_hdr().record_size);
                UndoLog undo;
                undo.is_deleted_ = old_meta.is_deleted_;
                undo.ts_ = old_meta.ts_;
                undo.tuple_test_ = old_data;
                auto prev_link = context_->txn_mgr_->GetVersionLink(rid);
                if (prev_link.has_value()) {
                    undo.prev_version_ = prev_link->prev_;
                }
                auto link = context_->txn_->AppendUndoLog(undo);
                context_->txn_mgr_->UpdateVersionLink(rid,
                    VersionUndoLink{link, true});
            }
            RmRecord rec_copy(fh_->get_file_hdr().record_size);
            memcpy(rec_copy.data, old_rec->data, rec_copy.size);

            // Remove from indexes first
            for (auto &index : tab_.indexes) {
                std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (!sm_manager_->ihs_.count(ix_name)) continue;
                auto ih = sm_manager_->ihs_.at(ix_name).get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(key + offset, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                // Register pending delete for INSERT conflict detection
                if (context_ && context_->txn_ && context_->txn_mgr_) {
                    context_->txn_mgr_->AddPendingDelete(fh_->GetFd(),
                        std::string(key, index.col_tot_len), context_->txn_->get_transaction_id());
                }
                ih->delete_entry(key, context_->txn_);
                delete[] key;
            }
            fh_->delete_record(rid, context_);

            // 记录写操作到事务
            if (context_ != nullptr && context_->txn_ != nullptr) {
                WriteRecord *wr = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, rec_copy);
                context_->txn_->append_write_record(wr);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};