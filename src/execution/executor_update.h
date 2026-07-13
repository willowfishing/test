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
        // 预初始化 set_clause 的值（只做一次，避免 init_raw assert 在循环内重复触发）
        std::vector<std::pair<int, std::shared_ptr<RmRecord>>> set_vals;
        // 记录自引用更新的信息: {target_offset, self_ref_col_offset, self_ref_col_type, rhs_val, rhs_raw}
        std::vector<std::tuple<int, int, ColType, Value, std::shared_ptr<RmRecord>>> self_ref_vals;
        for (auto &set_clause : set_clauses_) {
            if (set_clause.is_self_ref) {
                auto pos = AbstractExecutor::get_col(tab_.cols, set_clause.lhs);
                // Find self_ref_col in table
                auto ref_pos = AbstractExecutor::get_col(tab_.cols, {.tab_name = tab_name_, .col_name = set_clause.self_ref_col});
                Value rhs = set_clause.rhs;
                if (pos->type == TYPE_FLOAT && rhs.type == TYPE_INT) {
                    float fval = static_cast<float>(rhs.int_val);
                    rhs.set_float(fval);
                }
                rhs.init_raw(pos->len);
                self_ref_vals.push_back({pos->offset, ref_pos->offset, ref_pos->type, rhs, rhs.raw});
            } else {
                auto pos = AbstractExecutor::get_col(tab_.cols, set_clause.lhs);
                if (pos->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_INT) {
                    float fval = static_cast<float>(set_clause.rhs.int_val);
                    set_clause.rhs.set_float(fval);
                }
                set_clause.rhs.init_raw(pos->len);
                set_vals.push_back({pos->offset, set_clause.rhs.raw});
            }
        }

        // Sort rids for consistent lock ordering (deadlock prevention)
        std::sort(rids_.begin(), rids_.end(), [](const Rid &a, const Rid &b) {
            if (a.page_no != b.page_no) return a.page_no < b.page_no;
            return a.slot_no < b.slot_no;
        });
        // Phase 1: claim all records first (all-or-nothing to prevent deadlocks)
        std::vector<Rid> claimed;
        for (auto &rid : rids_) {
            if (context_ && context_->txn_ && context_->txn_mgr_) {
                TupleMeta meta = fh_->get_meta(rid);
                if (meta.writer_txn_ != INVALID_TXN_ID &&
                    meta.writer_txn_ != context_->txn_->get_transaction_id()) {
                    auto *writer = context_->txn_mgr_->get_transaction_safe(meta.writer_txn_);
                    if (writer && writer->get_state() == TransactionState::COMMITTED &&
                        writer->get_commit_ts() > context_->txn_->get_start_ts()) {
                        for (auto &cr : claimed)
                            context_->txn_mgr_->ReleaseWriteClaim(cr, context_->txn_->get_transaction_id());
                        throw TransactionAbortException(context_->txn_->get_transaction_id(),
                            AbortReason::DEADLOCK_PREVENTION);
                    }
                }
                if (!context_->txn_mgr_->TryClaimWrite(rid, context_->txn_->get_transaction_id())) {
                    for (auto &cr : claimed)
                        context_->txn_mgr_->ReleaseWriteClaim(cr, context_->txn_->get_transaction_id());
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
                claimed.push_back(rid);
            }
        }
        // Phase 2: write all records with SSI tracking
        for (auto &rid : rids_) {
            // SSI: rw-out dependency tracking and dangerous structure check
            if (context_ && context_->txn_ && context_->txn_mgr_ &&
                context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
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
                if (context_->txn_mgr_->CheckSSIDangerous(context_->txn_)) {
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
            }
            auto old_rec = fh_->get_record(rid, context_);
            // Save old version for MVCC version chain visibility
            if (context_ && context_->txn_ && context_->txn_mgr_) {
                TupleMeta old_meta = fh_->get_meta(rid);
                // Store old record bytes in the UndoLog's tuple_test_ field
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
            // Build new record
            RmRecord new_rec(fh_->get_file_hdr().record_size);
            memcpy(new_rec.data, old_rec->data, fh_->get_file_hdr().record_size);
            for (auto &sv : set_vals) {
                memcpy(new_rec.data + sv.first, sv.second->data, sv.second->size);
            }
            // Handle self-referencing updates: col = col + value
            for (auto &srv : self_ref_vals) {
                int target_off = std::get<0>(srv);
                int ref_off = std::get<1>(srv);
                ColType ref_type = std::get<2>(srv);
                Value &rhs_val = std::get<3>(srv);
                std::shared_ptr<RmRecord> rhs_raw = std::get<4>(srv);
                if (ref_type == TYPE_INT) {
                    int old_val = *(int *)(old_rec->data + ref_off);
                    int delta = rhs_val.int_val;
                    int new_val = old_val + delta;
                    memcpy(new_rec.data + target_off, &new_val, sizeof(int));
                } else if (ref_type == TYPE_FLOAT) {
                    float old_val = *(float *)(old_rec->data + ref_off);
                    float delta = rhs_val.float_val;
                    float new_val = old_val + delta;
                    memcpy(new_rec.data + target_off, &new_val, sizeof(float));
                }
            }

            // 先检查所有索引的唯一性冲突（old_key vs new_key）
            for (auto &index : tab_.indexes) {
                std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (!sm_manager_->ihs_.count(ix_name)) continue;
                auto ih = sm_manager_->ihs_.at(ix_name).get();

                char *old_key = new char[index.col_tot_len];
                char *new_key = new char[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(old_key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    memcpy(new_key + off, new_rec.data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                // 只有 key 变化时才检查冲突
                if (memcmp(old_key, new_key, index.col_tot_len) != 0) {
                    std::vector<Rid> result;
                    if (ih->get_value(new_key, &result, context_->txn_) && !result.empty()) {
                        delete[] old_key; delete[] new_key;
                        throw IncompatibleTypeError("Duplicate key in unique index", "");
                    }
                }
                delete[] old_key; delete[] new_key;
            }

            // 所有检查通过：删除旧索引 → 更新表 → 插入新索引
            for (auto &index : tab_.indexes) {
                std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (!sm_manager_->ihs_.count(ix_name)) continue;
                auto ih = sm_manager_->ihs_.at(ix_name).get();
                char *old_key = new char[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(old_key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                ih->delete_entry(old_key, context_->txn_);
                delete[] old_key;
            }
            fh_->update_record(rid, new_rec.data, context_);
            if (context_ != nullptr && context_->txn_ != nullptr) {
                RmRecord old_rec_copy(fh_->get_file_hdr().record_size);
                memcpy(old_rec_copy.data, old_rec->data, old_rec_copy.size);
                WriteRecord *wr = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, old_rec_copy);
                context_->txn_->append_write_record(wr);
                if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    context_->txn_->add_write_table(tab_name_);
                    context_->txn_mgr_->CheckAndAddPredicateDep(tab_name_, context_->txn_);
                }
            }
            for (auto &index : tab_.indexes) {
                std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (!sm_manager_->ihs_.count(ix_name)) continue;
                auto ih = sm_manager_->ihs_.at(ix_name).get();
                char *new_key = new char[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(new_key + off, new_rec.data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                ih->insert_entry(new_key, rid, context_->txn_);
                delete[] new_key;
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};