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

#include <algorithm>
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
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager, IsolationLevel isolation_level) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++, isolation_level);
    }
    txn->set_isolation_level(isolation_level);
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);

    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map[txn->get_transaction_id()] = txn;
    }

    running_txns_.AddTxn(txn->get_start_ts());

    // WAL: log BEGIN
    if (log_manager != nullptr) {
        auto *begin_log = new BeginLogRecord(txn->get_transaction_id());
        lsn_t lsn = log_manager->add_log_to_buffer(begin_log);
        txn->set_prev_lsn(lsn);
        delete begin_log;
    }

    // For SI/SER modes, capture snapshot at begin
    if (isolation_level == IsolationLevel::SNAPSHOT_ISOLATION ||
        isolation_level == IsolationLevel::SERIALIZABLE) {
        capture_snapshot(txn);
    }

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
    // WAL: log COMMIT before flushing
    if (log_manager != nullptr) {
        auto *commit_log = new CommitLogRecord(txn->get_transaction_id());
        commit_log->prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(commit_log);
        txn->set_prev_lsn(lsn);
        delete commit_log;
    }

    // Assign commit timestamp for MVCC
    timestamp_t commit_ts = next_timestamp_++;
    txn->set_commit_ts(commit_ts);
    running_txns_.UpdateCommitTs(commit_ts);

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

    // Remove txn from running transactions
    running_txns_.RemoveTxn(txn->get_start_ts());
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
    // WAL: log ABORT before flushing
    if (log_manager != nullptr) {
        auto *abort_log = new AbortLogRecord(txn->get_transaction_id());
        abort_log->prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(abort_log);
        txn->set_prev_lsn(lsn);
        delete abort_log;

        log_manager->flush_log_to_disk();
    }
    release_locks(lock_manager_, txn);

    // Clean up version chain info: mark any version links as no longer in progress
    {
        std::unique_lock<std::shared_mutex> lock(version_info_mutex_);
        for (auto &[page_id, page_info] : version_info_) {
            std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
            for (auto &[slot, version_link] : page_info->prev_version_) {
                if (version_link.prev_.prev_txn_ == txn->get_transaction_id()) {
                    version_link.in_progress_ = false;
                }
            }
        }
    }

    // Clean up SSI dependencies
    txn->clear_ssi_state();

    // Remove txn from running transactions
    running_txns_.RemoveTxn(txn->get_start_ts());
}

// ===== MVCC Version Chain Functions =====

bool TransactionManager::UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) {
    std::unique_lock<std::shared_mutex> lock(version_info_mutex_);

    auto &page_info_ptr = version_info_[rid.page_no];
    if (page_info_ptr == nullptr) {
        page_info_ptr = std::make_shared<PageVersionInfo>();
    }

    std::unique_lock<std::shared_mutex> page_lock(page_info_ptr->mutex_);

    if (check) {
        std::optional<UndoLink> current;
        auto it = page_info_ptr->prev_version_.find(rid.slot_no);
        if (it != page_info_ptr->prev_version_.end()) {
            current = UndoLink{it->second.prev_.prev_txn_, it->second.prev_.prev_log_idx_};
        }
        if (!check(current)) {
            return false;
        }
    }

    if (prev_link.has_value()) {
        VersionUndoLink vul;
        vul.prev_ = prev_link.value();
        vul.in_progress_ = true;
        page_info_ptr->prev_version_[rid.slot_no] = vul;
    } else {
        page_info_ptr->prev_version_.erase(rid.slot_no);
    }

    return true;
}

bool TransactionManager::UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    std::unique_lock<std::shared_mutex> lock(version_info_mutex_);

    auto &page_info_ptr = version_info_[rid.page_no];
    if (page_info_ptr == nullptr) {
        page_info_ptr = std::make_shared<PageVersionInfo>();
    }

    std::unique_lock<std::shared_mutex> page_lock(page_info_ptr->mutex_);

    if (check) {
        std::optional<VersionUndoLink> current;
        auto it = page_info_ptr->prev_version_.find(rid.slot_no);
        if (it != page_info_ptr->prev_version_.end()) {
            current = it->second;
        }
        if (!check(current)) {
            return false;
        }
    }

    if (prev_version.has_value()) {
        page_info_ptr->prev_version_[rid.slot_no] = prev_version.value();
    } else {
        page_info_ptr->prev_version_.erase(rid.slot_no);
    }

    return true;
}

std::optional<UndoLink> TransactionManager::GetUndoLink(Rid rid) {
    std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
    auto it = version_info_.find(rid.page_no);
    if (it == version_info_.end()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> page_lock(it->second->mutex_);
    auto slot_it = it->second->prev_version_.find(rid.slot_no);
    if (slot_it == it->second->prev_version_.end()) {
        return std::nullopt;
    }
    return UndoLink{slot_it->second.prev_.prev_txn_, slot_it->second.prev_.prev_log_idx_};
}

std::optional<VersionUndoLink> TransactionManager::GetVersionLink(Rid rid) {
    std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
    auto it = version_info_.find(rid.page_no);
    if (it == version_info_.end()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> page_lock(it->second->mutex_);
    auto slot_it = it->second->prev_version_.find(rid.slot_no);
    if (slot_it == it->second->prev_version_.end()) {
        return std::nullopt;
    }
    return slot_it->second;
}

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    if (link.prev_txn_ == INVALID_TXN_ID) {
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(latch_);
    auto it = txn_map.find(link.prev_txn_);
    if (it == txn_map.end()) {
        return std::nullopt;
    }
    return it->second->GetUndoLog(link.prev_log_idx_);
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto result = GetUndoLogOptional(link);
    if (!result.has_value()) {
        throw InternalError("GetUndoLog: no undo log found for txn " + std::to_string(link.prev_txn_));
    }
    return result.value();
}

timestamp_t TransactionManager::GetWatermark() {
    return running_txns_.GetWatermark();
}

void TransactionManager::GarbageCollection() {
    // Simple GC: nothing to clean for now
}

// ===== SSI (Serializable Snapshot Isolation) Functions =====

bool TransactionManager::DetectSSIDanger(Transaction *current_txn) {
    if (current_txn == nullptr) return false;
    std::unique_lock<std::mutex> lock(latch_);
    return DetectSSIDangerUnlocked(current_txn);
}

void TransactionManager::CheckSSIRWDependency(Transaction *current_txn,
                                               const std::string &tab_name, const Rid &rid) {
    if (current_txn == nullptr) return;
    if (current_txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) return;

    bool danger_found = false;
    {
        std::unique_lock<std::mutex> lock(latch_);
        for (auto &[other_id, other_txn] : txn_map) {
            if (other_txn == nullptr || other_txn == current_txn) continue;
            if (other_txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
            if (other_txn->get_state() != TransactionState::GROWING) continue;

            // Check if other_txn read this exact record
            const auto &read_set = other_txn->get_read_set();
            auto it = read_set.find(tab_name);
            if (it != read_set.end() && it->second.count(rid)) {
                other_txn->add_rw_dependency_out(current_txn->get_transaction_id());
                current_txn->add_rw_dependency_in(other_txn->get_transaction_id());
            }

            // Check predicate reads: does this write match scan conditions?
            const auto &pred_reads = other_txn->get_predicate_reads();
            for (const auto &pred : pred_reads) {
                if (pred.first == tab_name) {
                    other_txn->add_rw_dependency_out(current_txn->get_transaction_id());
                    current_txn->add_rw_dependency_in(other_txn->get_transaction_id());
                    break;
                }
            }
        }
        // Check danger inside the lock to avoid concurrent dependency changes
        danger_found = DetectSSIDangerUnlocked(current_txn);
    }

    if (danger_found) {
        throw TransactionAbortException(current_txn->get_transaction_id(), AbortReason::SSI_DANGER);
    }
}

bool TransactionManager::DetectSSIDangerUnlocked(Transaction *current_txn) {
    if (current_txn == nullptr) return false;
    const auto &rw_in = current_txn->get_rw_in();
    const auto &rw_out = current_txn->get_rw_out();
    for (txn_id_t t_in_id : rw_in) {
        auto it_in = txn_map.find(t_in_id);
        if (it_in == txn_map.end() || it_in->second == nullptr) continue;
        Transaction *t_in = it_in->second;
        if (t_in->get_state() != TransactionState::GROWING) continue;
        for (txn_id_t t_out_id : rw_out) {
            if (t_in_id == t_out_id) return true;
            auto it_out = txn_map.find(t_out_id);
            if (it_out == txn_map.end() || it_out->second == nullptr) continue;
            Transaction *t_out = it_out->second;
            if (t_out->get_state() == TransactionState::COMMITTED &&
                t_out->get_commit_ts() < t_in->get_start_ts()) return true;
        }
    }
    return false;
}

void TransactionManager::CheckSSIInvisibleWrite(Transaction *reader_txn,
                                                 const std::string &tab_name, const Rid &rid,
                                                 const RmRecord &record) {
    if (reader_txn == nullptr) return;
    if (reader_txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) return;

    // Check write_set_ of other active SER txns for invisible writes
    std::unique_lock<std::mutex> lock(latch_);
    for (auto &[other_id, other_txn] : txn_map) {
        if (other_txn == nullptr || other_txn == reader_txn) continue;
        if (other_txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
        if (other_txn->get_state() != TransactionState::GROWING) continue;

        auto write_set = other_txn->get_write_set();
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            WriteRecord *wr = *it;
            if (wr->GetTableName() == tab_name && wr->GetRid() == rid) {
                // other_txn has written this record → invisible write from reader's perspective
                reader_txn->add_rw_dependency_out(other_id);
                other_txn->add_rw_dependency_in(reader_txn->get_transaction_id());
                break;
            }
        }
    }
    lock.unlock();

    if (DetectSSIDanger(reader_txn)) {
        throw TransactionAbortException(reader_txn->get_transaction_id(), AbortReason::SSI_DANGER);
    }
}

bool TransactionManager::HasWriteConflict(Transaction *txn, Rid rid, const std::string &tab_name) {
    if (txn == nullptr) return false;
    if (txn->get_isolation_level() != IsolationLevel::SNAPSHOT_ISOLATION &&
        txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return false;
    }

    auto version_link = GetVersionLink(rid);
    if (!version_link.has_value() || !version_link->prev_.IsValid()) {
        return false;
    }

    // Check if another txn has an in-progress write
    if (version_link->in_progress_) {
        txn_id_t writer_id = version_link->prev_.prev_txn_;
        if (writer_id != txn->get_transaction_id()) {
            return true;
        }
    }

    // Check if version was committed after our snapshot
    txn_id_t writer_id = version_link->prev_.prev_txn_;
    if (writer_id != INVALID_TXN_ID && writer_id != txn->get_transaction_id()) {
        std::unique_lock<std::mutex> lock(latch_);
        auto it = txn_map.find(writer_id);
        if (it != txn_map.end()) {
            Transaction *writer_txn = it->second;
            timestamp_t writer_commit_ts = writer_txn->get_commit_ts();
            if (writer_commit_ts > txn->get_start_ts()) {
                return true;
            }
        }
    }

    return false;
}
