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
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"

#include <vector>

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {
std::unique_ptr<char[]> build_index_key(const IndexMeta &index, const char *record_data) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(key.get() + offset, record_data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

void delete_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                          const RmRecord &record, Transaction *txn) {
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = build_index_key(index, record.data);
        ih->delete_entry(key.get(), txn);
    }
}

void insert_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                          const RmRecord &record, const Rid &rid, Transaction *txn) {
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = build_index_key(index, record.data);
        ih->insert_entry(key.get(), rid, txn);
    }
}

void release_locks(LockManager *lock_manager, Transaction *txn) {
    if (lock_manager == nullptr || txn == nullptr) {
        return;
    }
    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (const auto &lock_data_id : locks) {
        lock_manager->unlock(txn, lock_data_id);
    }
}
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::capture_snapshot(Transaction *txn) {
    if (txn == nullptr || sm_manager_ == nullptr) {
        return;
    }
    txn->clear_snapshot();
    for (auto &entry : sm_manager_->fhs_) {
        const std::string &tab_name = entry.first;
        auto &fh = entry.second;
        Transaction::SnapshotRecords records;
        for (RmScan scan(fh.get()); !scan.is_end(); scan.next()) {
            Rid rid = scan.rid();
            auto record = fh->get_record(rid, nullptr);
            records.emplace_back(rid, *record);
        }
        txn->set_snapshot_records(tab_name, std::move(records));
    }

    std::unique_lock<std::mutex> lock(latch_);
    for (auto &entry : txn_map) {
        Transaction *active_txn = entry.second;
        if (active_txn == nullptr || active_txn == txn || active_txn->get_state() != TransactionState::GROWING) {
            continue;
        }
        auto write_set = active_txn->get_write_set();
        for (auto iter = write_set->rbegin(); iter != write_set->rend(); ++iter) {
            WriteRecord *write_record = *iter;
            const std::string tab_name = write_record->GetTableName();
            Rid rid = write_record->GetRid();
            if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
                txn->remove_snapshot_record(tab_name, rid);
            } else if (write_record->GetWriteType() == WType::DELETE_TUPLE ||
                       write_record->GetWriteType() == WType::UPDATE_TUPLE) {
                txn->upsert_snapshot_record(tab_name, rid, write_record->GetRecord());
            }
        }
    }
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    txn->set_state(TransactionState::COMMITTED);
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    if (sm_manager_ != nullptr) {
        for (auto &entry : sm_manager_->fhs_) {
            entry.second->flush();
        }
        for (auto &entry : sm_manager_->ihs_) {
            entry.second->flush();
        }
    }
    release_locks(lock_manager_, txn);
    txn->get_write_set()->clear();
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();
        const std::string tab_name = write_record->GetTableName();
        auto &fh = sm_manager_->fhs_.at(tab_name);
        TabMeta tab = sm_manager_->db_.get_table(tab_name);
        Rid rid = write_record->GetRid();

        if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
            auto inserted = fh->get_record(rid, nullptr);
            delete_index_entries(sm_manager_, tab, tab_name, *inserted, txn);
            fh->delete_record(rid, nullptr);
        } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_record = write_record->GetRecord();
            fh->insert_record(rid, old_record.data);
            insert_index_entries(sm_manager_, tab, tab_name, old_record, rid, txn);
        } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
            auto current = fh->get_record(rid, nullptr);
            delete_index_entries(sm_manager_, tab, tab_name, *current, txn);
            RmRecord &old_record = write_record->GetRecord();
            fh->update_record(rid, old_record.data, nullptr);
            insert_index_entries(sm_manager_, tab, tab_name, old_record, rid, txn);
        }
        delete write_record;
    }
    txn->set_state(TransactionState::ABORTED);
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    release_locks(lock_manager_, txn);
}
