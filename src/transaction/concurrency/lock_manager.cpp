/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_exclusive_on_record(txn, rid, tab_fd);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) {
        return true;
    }
    std::lock_guard<std::mutex> lock(latch_);
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];
    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn->get_transaction_id() &&
            request.lock_mode_ == LockMode::EXLUCSIVE) {
            return true;
        }
        if (request.granted_ && request.txn_id_ != txn->get_transaction_id()) {
            return false;
        }
    }
    queue.request_queue_.emplace_back(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }
    std::lock_guard<std::mutex> lock(latch_);
    auto iter = lock_table_.find(lock_data_id);
    if (iter == lock_table_.end()) {
        return true;
    }
    auto &queue = iter->second.request_queue_;
    queue.remove_if([&](const LockRequest &request) {
        return request.txn_id_ == txn->get_transaction_id();
    });
    txn->get_lock_set()->erase(lock_data_id);
    if (queue.empty()) {
        lock_table_.erase(iter);
    }
    return true;
}
