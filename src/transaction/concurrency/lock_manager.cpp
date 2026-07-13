/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY or FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_set>

LockManager::GroupLockMode LockManager::recompute_group_lock_mode(const LockRequestQueue &queue) const {
    bool has_exclusive = false;
    bool has_shared = false;
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        if (request.lock_mode_ == LockMode::EXLUCSIVE) {
            has_exclusive = true;
            break;
        }
        if (request.lock_mode_ == LockMode::SHARED) {
            has_shared = true;
        }
    }
    return has_exclusive ? GroupLockMode::X
         : has_shared   ? GroupLockMode::S
                        : GroupLockMode::NON_LOCK;
}

bool LockManager::can_grant_request(const LockRequestQueue &queue,
                                    std::list<LockRequest>::const_iterator request_iter) const {
    if (request_iter == queue.request_queue_.end()) {
        return false;
    }
    if (request_iter->granted_) {
        return true;
    }

    const txn_id_t txn_id = request_iter->txn_id_;
    if (request_iter->lock_mode_ == LockMode::SHARED) {
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                return false;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                return false;
            }
        }
        return true;
    }

    for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
        if (iter->txn_id_ != txn_id) {
            return false;
        }
    }
    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ != txn_id) {
            return false;
        }
    }
    return true;
}

void LockManager::notify_grantable_waiters(LockRequestQueue &queue) const {
    for (auto iter = queue.request_queue_.begin(); iter != queue.request_queue_.end(); ++iter) {
        if (iter->granted_) {
            continue;
        }
        if (!can_grant_request(queue, iter)) {
            if (iter->lock_mode_ == LockMode::EXLUCSIVE) {
                break;
            }
            continue;
        }
        iter->cv_.notify_one();
        if (iter->lock_mode_ == LockMode::EXLUCSIVE) {
            break;
        }
    }
}

size_t LockManager::shard_index(lock_data_key_t lock_key) {
    lock_key ^= lock_key >> 33;
    lock_key *= 0xff51afd7ed558ccdULL;
    lock_key ^= lock_key >> 33;
    return static_cast<size_t>(lock_key & (LOCK_SHARD_COUNT - 1));
}

LockManager::LockTableShard &LockManager::shard_for(lock_data_key_t lock_key) {
    return lock_table_shards_[shard_index(lock_key)];
}

bool LockManager::set_wait_edges_and_detect_cycle(txn_id_t waiting_txn,
                                                  const std::vector<txn_id_t> &blockers) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    if (blockers.empty()) {
        wait_edges_.erase(waiting_txn);
        return false;
    }

    auto &edges = wait_edges_[waiting_txn];
    edges.clear();
    edges.reserve(blockers.size());
    for (auto blocker : blockers) {
        if (blocker != waiting_txn &&
            std::find(edges.begin(), edges.end(), blocker) == edges.end()) {
            edges.push_back(blocker);
        }
    }

    std::unordered_set<txn_id_t> visited;
    std::function<bool(txn_id_t)> reaches_waiting = [&](txn_id_t current) {
        if (current == waiting_txn) {
            return true;
        }
        if (!visited.insert(current).second) {
            return false;
        }
        auto iter = wait_edges_.find(current);
        if (iter == wait_edges_.end()) {
            return false;
        }
        for (auto next : iter->second) {
            if (reaches_waiting(next)) {
                return true;
            }
        }
        return false;
    };

    for (auto blocker : edges) {
        if (reaches_waiting(blocker)) {
            wait_edges_.erase(waiting_txn);
            return true;
        }
    }
    return false;
}

void LockManager::clear_wait_edges(txn_id_t txn_id) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    wait_edges_.erase(txn_id);
}

void LockManager::remove_txn_from_wait_graph(txn_id_t txn_id) {
    std::lock_guard<std::mutex> guard(wait_graph_latch_);
    wait_edges_.erase(txn_id);
    for (auto iter = wait_edges_.begin(); iter != wait_edges_.end();) {
        auto &edges = iter->second;
        edges.erase(std::remove(edges.begin(), edges.end(), txn_id), edges.end());
        if (edges.empty()) {
            iter = wait_edges_.erase(iter);
        } else {
            ++iter;
        }
    }
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn == nullptr) {
        return true;
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    std::unique_lock<std::mutex> lock(shard.latch_);
    auto &queue = shard.lock_table_[lock_key];
    const auto txn_id = txn->get_transaction_id();

    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn_id &&
            (request.lock_mode_ == LockMode::SHARED || request.lock_mode_ == LockMode::EXLUCSIVE)) {
            return true;
        }
    }

    queue.request_queue_.emplace_back(txn_id, LockMode::SHARED);
    auto request_iter = std::prev(queue.request_queue_.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    auto has_blocker = [&]() {
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                return true;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                return true;
            }
        }
        return false;
    };
    auto collect_blockers = [&]() {
        std::vector<txn_id_t> blockers;
        blockers.reserve(queue.request_queue_.size());
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id && iter->lock_mode_ == LockMode::EXLUCSIVE) {
                blockers.push_back(iter->txn_id_);
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id &&
                request.lock_mode_ == LockMode::EXLUCSIVE) {
                blockers.push_back(request.txn_id_);
            }
        }
        return blockers;
    };
    auto can_grant = [&]() {
        if (txn->get_state() == TransactionState::ABORTED) {
            return false;
        }
        return !has_blocker();
    };

    while (!can_grant()) {
        if (set_wait_edges_and_detect_cycle(txn_id, collect_blockers())) {
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty()) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
        if (!request_iter->cv_.wait_until(lock, deadline, can_grant)) {
            clear_wait_edges(txn_id);
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty()) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
    }

    clear_wait_edges(txn_id);
    request_iter->granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::S;
    txn->add_lock(lock_key);
    return true;
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

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    std::unique_lock<std::mutex> lock(shard.latch_);
    auto &queue = shard.lock_table_[lock_key];
    const auto txn_id = txn->get_transaction_id();

    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn_id &&
            request.lock_mode_ == LockMode::EXLUCSIVE) {
            return true;
        }
    }

    if (txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
        txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        for (const auto &request : queue.request_queue_) {
            if (request.txn_id_ != txn_id) {
                return false;
            }
        }
    }

    queue.request_queue_.emplace_back(txn_id, LockMode::EXLUCSIVE);
    auto request_iter = std::prev(queue.request_queue_.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    auto has_blocker = [&]() {
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id) {
                return true;
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id) {
                return true;
            }
        }
        return false;
    };
    auto collect_blockers = [&]() {
        std::vector<txn_id_t> blockers;
        blockers.reserve(queue.request_queue_.size());
        for (auto iter = queue.request_queue_.begin(); iter != request_iter; ++iter) {
            if (iter->txn_id_ != txn_id) {
                blockers.push_back(iter->txn_id_);
            }
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn_id) {
                blockers.push_back(request.txn_id_);
            }
        }
        return blockers;
    };
    auto can_grant = [&]() {
        if (txn->get_state() == TransactionState::ABORTED) {
            return false;
        }
        return !has_blocker();
    };

    while (!can_grant()) {
        if (set_wait_edges_and_detect_cycle(txn_id, collect_blockers())) {
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty()) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
        if (!request_iter->cv_.wait_until(lock, deadline, can_grant)) {
            clear_wait_edges(txn_id);
            queue.request_queue_.erase(request_iter);
            if (queue.request_queue_.empty()) {
                shard.lock_table_.erase(lock_key);
            } else {
                notify_grantable_waiters(queue);
            }
            return false;
        }
    }

    clear_wait_edges(txn_id);
    request_iter->granted_ = true;
    queue.group_lock_mode_ = GroupLockMode::X;
    txn->add_lock(lock_key);
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
 * @param {Transaction*} txn 要释放的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    const auto lock_key = lock_data_id.Get();
    auto &shard = shard_for(lock_key);
    const auto txn_id = txn->get_transaction_id();
    {
        std::lock_guard<std::mutex> lock(shard.latch_);
        auto iter = shard.lock_table_.find(lock_key);
        if (iter == shard.lock_table_.end()) {
            txn->remove_lock(lock_key);
            return true;
        }
        auto &queue_info = iter->second;
        auto &queue = queue_info.request_queue_;
        const size_t before = queue.size();
        queue.remove_if([&](const LockRequest &request) {
            return request.txn_id_ == txn_id;
        });
        txn->remove_lock(lock_key);
        if (queue.size() == before) {
            return true;
        }
        if (queue.empty()) {
            shard.lock_table_.erase(iter);
        } else {
            queue_info.group_lock_mode_ = recompute_group_lock_mode(queue_info);
            notify_grantable_waiters(queue_info);
        }
    }
    remove_txn_from_wait_graph(txn_id);
    return true;
}

bool LockManager::unlock_all(Transaction *txn) {
    if (txn == nullptr) {
        return true;
    }
    const auto &lock_set = txn->get_lock_set();
    if (lock_set.empty()) {
        remove_txn_from_wait_graph(txn->get_transaction_id());
        return true;
    }

    const auto txn_id = txn->get_transaction_id();
    auto release_one = [&](LockTableShard &shard, lock_data_key_t lock_key) {
        auto iter = shard.lock_table_.find(lock_key);
        if (iter == shard.lock_table_.end()) {
            return;
        }
        auto &queue_info = iter->second;
        auto &queue = queue_info.request_queue_;
        const size_t before = queue.size();
        queue.remove_if([&](const LockRequest &request) {
            return request.txn_id_ == txn_id;
        });
        if (queue.size() == before) {
            return;
        }
        if (queue.empty()) {
            shard.lock_table_.erase(iter);
        } else {
            queue_info.group_lock_mode_ = recompute_group_lock_mode(queue_info);
            notify_grantable_waiters(queue_info);
        }
    };

    std::vector<lock_data_key_t> locks = lock_set;
    std::sort(locks.begin(), locks.end(), [](lock_data_key_t lhs, lock_data_key_t rhs) {
        const auto lhs_shard = shard_index(lhs);
        const auto rhs_shard = shard_index(rhs);
        return lhs_shard == rhs_shard ? lhs < rhs : lhs_shard < rhs_shard;
    });
    locks.erase(std::unique(locks.begin(), locks.end()), locks.end());

    for (size_t begin = 0; begin < locks.size();) {
        const auto shard_id = shard_index(locks[begin]);
        size_t end = begin + 1;
        while (end < locks.size() && shard_index(locks[end]) == shard_id) {
            ++end;
        }

        auto &shard = lock_table_shards_[shard_id];
        std::lock_guard<std::mutex> lock(shard.latch_);
        for (size_t pos = begin; pos < end; ++pos) {
            release_one(shard, locks[pos]);
        }
        begin = end;
    }

    txn->clear_locks();
    remove_txn_from_wait_graph(txn_id);
    return true;
}
