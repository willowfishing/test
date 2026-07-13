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
#include "system/sm_manager.h"
#include "index/ix.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

// Version link management
std::optional<VersionUndoLink> TransactionManager::GetVersionLink(Rid rid) {
    std::shared_ptr<PageVersionInfo> page_info;
    {
        std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
        auto it = version_info_.find(rid.page_no);
        if (it == version_info_.end()) return std::nullopt;
        page_info = it->second;
    }
    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    auto vit = page_info->prev_version_.find(rid.slot_no);
    if (vit != page_info->prev_version_.end()) return vit->second;
    return std::nullopt;
}

bool TransactionManager::UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    std::shared_ptr<PageVersionInfo> page_info;
    {
        std::unique_lock<std::shared_mutex> wlock(version_info_mutex_);
        auto it = version_info_.find(rid.page_no);
        if (it == version_info_.end()) {
            page_info = std::make_shared<PageVersionInfo>();
            version_info_[rid.page_no] = page_info;
        } else {
            page_info = it->second;
        }
    }
    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    auto it = page_info->prev_version_.find(rid.slot_no);
    std::optional<VersionUndoLink> current;
    if (it != page_info->prev_version_.end()) current = it->second;
    if (check && !check(current)) return false;
    if (prev_version.has_value()) {
        page_info->prev_version_[rid.slot_no] = *prev_version;
    } else {
        page_info->prev_version_.erase(rid.slot_no);
    }
    return true;
}

std::optional<UndoLink> TransactionManager::GetUndoLink(Rid rid) {
    auto vl = GetVersionLink(rid);
    if (vl.has_value()) return vl->prev_;
    return std::nullopt;
}

bool TransactionManager::UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) {
    return UpdateVersionLink(rid, VersionUndoLink::FromOptionalUndoLink(prev_link),
        [&](std::optional<VersionUndoLink> current) {
            if (check) {
                std::optional<UndoLink> current_undo;
                if (current.has_value()) current_undo = current->prev_;
                return check(current_undo);
            }
            return true;
        });
}

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    auto it = txn_map.find(link.prev_txn_);
    if (it == txn_map.end()) return std::nullopt;
    return it->second->GetUndoLog(link.prev_log_idx_);
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto result = GetUndoLogOptional(link);
    if (!result.has_value()) throw InternalError("UndoLog not found");
    return *result;
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
    }
    txn->set_state(TransactionState::GROWING);
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        timestamp_t start_ts = next_timestamp_++;
        txn->set_start_ts(start_ts);
        running_txns_.AddTxn(start_ts);
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    lock.unlock();
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    txn->set_state(TransactionState::COMMITTED);
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        timestamp_t commit_ts = next_timestamp_++;
        txn->set_commit_ts(commit_ts);
        last_commit_ts_ = commit_ts;
        running_txns_.UpdateCommitTs(commit_ts);
        running_txns_.RemoveTxn(txn->get_start_ts());
        // Release write claims on all written records
        for (auto &wr : *txn->get_write_set()) {
            ReleaseWriteClaim(wr->GetRid(), txn->get_transaction_id());
        }
        // Clean up pending deletes
        {
            std::lock_guard<std::mutex> lock(pending_deletes_mutex_);
            for (auto &[key, txn_set] : pending_deletes_) {
                txn_set.erase(txn->get_transaction_id());
            }
        }
        // Clean up global read set
        RemoveTxnFromGlobalReadSet(txn->get_transaction_id());
        if (log_manager) {
            log_manager->flush_log_to_disk();
        }
        // Keep txn in txn_map (don't erase after MVCC return)
        return;
    }
    // 释放所有锁 (2PL mode)
    if (lock_manager_) {
        for (auto &lock_id : *txn->get_lock_set()) {
            lock_manager_->unlock(txn, lock_id);
        }
    }
    txn->get_lock_set()->clear();
    // 将事务日志刷入磁盘
    if (log_manager) {
        log_manager->flush_log_to_disk();
    }
    // 从全局事务表中移除
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}

void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // 回滚所有写操作（逆序）
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *wr = write_set->back();
        // 只有表仍然存在时才回滚（表可能已被 drop）
        if (sm_manager_->fhs_.count(wr->GetTableName())) {
            auto fh = sm_manager_->fhs_.at(wr->GetTableName()).get();
            auto &tab_name = wr->GetTableName();
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);

            if (wr->GetWriteType() == WType::INSERT_TUPLE) {
                // 回滚插入：先读取记录用于计算索引键，再删除索引项，最后删除记录
                auto rec = fh->get_record(wr->GetRid(), nullptr);
                // 删除所有索引项
                for (auto &index : tab.indexes) {
                    std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                    if (!sm_manager_->ihs_.count(ix_name)) continue;
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < index.col_num; ++j) {
                        memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->delete_entry(key, txn);
                    delete[] key;
                }
                fh->delete_record(wr->GetRid(), nullptr);
            } else if (wr->GetWriteType() == WType::DELETE_TUPLE) {
                // 回滚删除：重新插入记录，再重新插入索引项
                fh->insert_record(wr->GetRid(), wr->GetRecord().data);
                for (auto &index : tab.indexes) {
                    std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                    if (!sm_manager_->ihs_.count(ix_name)) continue;
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < index.col_num; ++j) {
                        memcpy(key + offset, wr->GetRecord().data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->insert_entry(key, wr->GetRid(), txn);
                    delete[] key;
                }
            } else if (wr->GetWriteType() == WType::UPDATE_TUPLE) {
                // 回滚更新：先读取当前（新）记录，删除新索引键，恢复旧记录，插入旧索引键
                auto new_rec = fh->get_record(wr->GetRid(), nullptr);
                // 删除新记录的索引项
                for (auto &index : tab.indexes) {
                    std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                    if (!sm_manager_->ihs_.count(ix_name)) continue;
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    char *new_key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < index.col_num; ++j) {
                        memcpy(new_key + offset, new_rec->data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->delete_entry(new_key, txn);
                    delete[] new_key;
                }
                // 恢复旧记录
                fh->update_record(wr->GetRid(), wr->GetRecord().data, nullptr);
                // 插入旧记录的索引项
                for (auto &index : tab.indexes) {
                    std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                    if (!sm_manager_->ihs_.count(ix_name)) continue;
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    char *old_key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t j = 0; j < index.col_num; ++j) {
                        memcpy(old_key + offset, wr->GetRecord().data + index.cols[j].offset, index.cols[j].len);
                        offset += index.cols[j].len;
                    }
                    ih->insert_entry(old_key, wr->GetRid(), txn);
                    delete[] old_key;
                }
            }
        }
        write_set->pop_back();
    }
    txn->set_state(TransactionState::ABORTED);
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.RemoveTxn(txn->get_start_ts());
        // Release write claims for all written records
        for (auto &wr : *txn->get_write_set()) {
            ReleaseWriteClaim(wr->GetRid(), txn->get_transaction_id());
        }
        // Clean up pending deletes
        {
            std::lock_guard<std::mutex> lock(pending_deletes_mutex_);
            for (auto &[key, txn_set] : pending_deletes_) {
                txn_set.erase(txn->get_transaction_id());
            }
        }
        RemoveTxnFromGlobalReadSet(txn->get_transaction_id());
        if (log_manager) {
            log_manager->flush_log_to_disk();
        }
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
        lock.unlock();
        return;
    }
    // 释放所有锁 (2PL mode)
    if (lock_manager_) {
        for (auto &lock_id : *txn->get_lock_set()) {
            lock_manager_->unlock(txn, lock_id);
        }
    }
    txn->get_lock_set()->clear();
    // 将事务日志刷入磁盘
    if (log_manager) {
        log_manager->flush_log_to_disk();
    }
    // 从全局事务表中移除
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}