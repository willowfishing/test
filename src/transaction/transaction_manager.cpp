/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <cstring>
#include <vector>

#include "index/ix.h"
#include "transaction/mvcc_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};
namespace {
thread_local IsolationLevel thread_isolation_level = IsolationLevel::SERIALIZABLE;
}

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id, thread_isolation_level);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);
    MvccManager::Begin(txn);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    timestamp_t commit_ts = next_timestamp_++;
    last_commit_ts_ = commit_ts;
    txn->set_commit_ts(commit_ts);
    MvccManager::OnCommit(txn, commit_ts);
    txn->set_state(TransactionState::COMMITTED);
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
    txn->get_lock_set()->clear();
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    auto make_key = [](const IndexMeta &index, const RmRecord &rec) {
        std::vector<char> key(index.col_tot_len);
        int offset = 0;
        for (int i = 0; i < index.col_num; ++i) {
            memcpy(key.data() + offset, rec.data + index.cols[i].offset, index.cols[i].len);
            offset += index.cols[i].len;
        }
        return key;
    };

    auto delete_indexes = [&](const std::string &tab_name, const TabMeta &tab, const RmRecord &rec) {
        for (auto &index : tab.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
            auto key = make_key(index, rec);
            ih->delete_entry(key.data(), txn);
        }
    };

    auto insert_indexes = [&](const std::string &tab_name, const TabMeta &tab, const RmRecord &rec, const Rid &rid) {
        for (auto &index : tab.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
            auto key = make_key(index, rec);
            ih->insert_entry(key.data(), rid, txn);
        }
    };

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();

        const std::string tab_name = write_record->GetTableName();
        TabMeta &tab = sm_manager_->db_.get_table(tab_name);
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        Rid rid = write_record->GetRid();

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                try {
                    auto rec = fh->get_record(rid, nullptr);
                    delete_indexes(tab_name, tab, *rec);
                    fh->delete_record(rid, nullptr);
                } catch (RecordNotFoundError &) {
                }
                break;
            }
            case WType::DELETE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                fh->insert_record(rid, old_rec.data);
                insert_indexes(tab_name, tab, old_rec, rid);
                break;
            }
            case WType::UPDATE_TUPLE: {
                RmRecord &old_rec = write_record->GetRecord();
                auto current_rec = fh->get_record(rid, nullptr);
                delete_indexes(tab_name, tab, *current_rec);
                fh->update_record(rid, old_rec.data, nullptr);
                insert_indexes(tab_name, tab, old_rec, rid);
                break;
            }
        }
        delete write_record;
    }
    MvccManager::OnAbort(txn);
    txn->set_state(TransactionState::ABORTED);
    txn->get_lock_set()->clear();
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
}

void TransactionManager::set_thread_isolation_level(IsolationLevel isolation_level) {
    thread_isolation_level = isolation_level;
}

IsolationLevel TransactionManager::get_thread_isolation_level() {
    return thread_isolation_level;
}
