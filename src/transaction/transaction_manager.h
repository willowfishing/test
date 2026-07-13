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

#include <atomic>
#include <unordered_map>
#include <optional>
#include <functional>
#include <shared_mutex>

#include "transaction.h"
#include "watermark.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "system/sm_manager.h"
#include "common/exception.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };

/// 版本链中的第一个撤销链接，将表堆元组链接到撤销日志。
struct VersionUndoLink {
    /** 版本链中的下一个版本。 */
    UndoLink prev_;
    bool in_progress_{false};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

class TransactionManager{
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                             ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }
    
    ~TransactionManager() = default;

    Transaction* begin(Transaction* txn, LogManager* log_manager);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager* get_lock_manager() { return lock_manager_; }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */    
    Transaction* get_transaction(txn_id_t txn_id) {
        if(txn_id == INVALID_TXN_ID) return nullptr;

        std::unique_lock<std::mutex> lock(latch_);
        auto it = TransactionManager::txn_map.find(txn_id);
        if (it == TransactionManager::txn_map.end()) {
            return nullptr;
        }
        auto *res = it->second;
        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    // Cross-thread safe transaction lookup (no thread_id assert)
    Transaction* get_transaction_safe(txn_id_t txn_id) {
        if(txn_id == INVALID_TXN_ID) return nullptr;
        std::unique_lock<std::mutex> lock(latch_);
        auto it = TransactionManager::txn_map.find(txn_id);
        if (it == TransactionManager::txn_map.end()) return nullptr;
        return it->second;
    }

    static std::unordered_map<txn_id_t, Transaction *> txn_map;     // 全局事务表，存放事务ID与事务对象的映射关系
    std::shared_mutex txn_map_mutex_;
    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /**
    * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
    * 在更新之前，将调用 `check` 函数以确保有效性。
    */
    bool UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(Rid rid);

    /** @brief Try to claim write intent on a record. Returns false if another txn already claimed it. */
    bool TryClaimWrite(const Rid &rid, txn_id_t txn_id) {
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
        if (it != page_info->prev_version_.end() && it->second.in_progress_) {
            if (it->second.prev_.prev_txn_ != txn_id) {
                // Check if holder is still active
                auto *holder = get_transaction_safe(it->second.prev_.prev_txn_);
                if (!holder || holder->get_state() == TransactionState::COMMITTED ||
                    holder->get_state() == TransactionState::ABORTED) {
                    // Holder is gone — clear the stale claim and proceed
                    it->second.in_progress_ = false;
                } else {
                    return false;
                }
            } else {
                return true;
            }
        }
        VersionUndoLink claim;
        claim.in_progress_ = true;
        claim.prev_.prev_txn_ = txn_id;
        page_info->prev_version_[rid.slot_no] = claim;
        return true;
    }

    /** @brief Release write intent on a record after commit/abort. */
    void ReleaseWriteClaim(const Rid &rid, txn_id_t txn_id) {
        std::shared_ptr<PageVersionInfo> page_info;
        {
            std::unique_lock<std::shared_mutex> wlock(version_info_mutex_);
            auto it = version_info_.find(rid.page_no);
            if (it == version_info_.end()) return;
            page_info = it->second;
        }
        std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
        auto vit = page_info->prev_version_.find(rid.slot_no);
        if (vit != page_info->prev_version_.end() && vit->second.in_progress_ &&
            vit->second.prev_.prev_txn_ == txn_id) {
            vit->second.in_progress_ = false;
        }
    }

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief Check if a tuple version is visible to the given transaction under MVCC. */
    bool IsTupleVisible(const TupleMeta &meta, Transaction *txn) const {
        if (txn == nullptr) return true;  // no transaction context → all visible
        // Own write is always visible
        if (meta.writer_txn_ == txn->get_transaction_id()) return true;
        // Committed by another transaction: visible if committed before my snapshot
        auto it = txn_map.find(meta.writer_txn_);
        if (it != txn_map.end()) {
            Transaction *writer = it->second;
            if (writer->get_state() == TransactionState::COMMITTED) {
                // Committed → check if commit_ts <= my snapshot
                if (writer->get_commit_ts() <= txn->get_start_ts() && !meta.is_deleted_) {
                    return true;
                }
            }
            // In-progress or aborted → not visible
        }
        // Writer not found (perhaps aborted and cleaned up) → check by timestamp
        if (meta.ts_ <= txn->get_start_ts() && !meta.is_deleted_ && meta.writer_txn_ == INVALID_TXN_ID) {
            return true;
        }
        return false;
    }

    /** @brief Walk version chain to find a record version visible to the given txn. */
    std::unique_ptr<RmRecord> GetVisibleVersion(const Rid &rid, RmFileHandle *fh, Transaction *txn) {
        try {
            auto [slot_meta, slot_rec] = fh->get_meta_and_record(rid);
            if (slot_meta.writer_txn_ == txn->get_transaction_id()) {
                return std::move(slot_rec);
            }
            if (IsTupleVisible(slot_meta, txn)) {
                return std::move(slot_rec);
            }
            // If slot version was committed before our snapshot, use the slot directly
            // (don't walk undo chain — the slot IS the visible committed state)
            if (slot_meta.writer_txn_ != INVALID_TXN_ID &&
                slot_meta.writer_txn_ != txn->get_transaction_id()) {
                auto it = txn_map.find(slot_meta.writer_txn_);
                if (it != txn_map.end() && it->second->get_state() == TransactionState::COMMITTED &&
                    it->second->get_commit_ts() <= txn->get_start_ts()) {
                    if (slot_meta.is_deleted_) return nullptr;  // deleted in snapshot
                    return std::move(slot_rec);  // committed version visible
                }
            }
            auto vlink = GetVersionLink(rid);
            if (!vlink.has_value()) return nullptr;
            UndoLink link = vlink->prev_;
            int max_steps = 10;
            while (link.IsValid() && max_steps-- > 0) {
                auto undo = GetUndoLogOptional(link);
                if (!undo.has_value()) break;
                if (undo->ts_ <= txn->get_start_ts()) {
                    if (undo->is_deleted_) return nullptr;
                    if (undo->tuple_test_ != nullptr && undo->tuple_test_->size == fh->get_file_hdr().record_size) {
                        auto rec = std::make_unique<RmRecord>(fh->get_file_hdr().record_size);
                        memcpy(rec->data, undo->tuple_test_->data, fh->get_file_hdr().record_size);
                        return rec;
                    }
                    return nullptr;
                }
                link = undo->prev_version_;
            }
            return nullptr;
        } catch (...) {
            return nullptr;  // don't crash on version chain errors
        }
    }

    // SSI: check predicate reads when inserting/updating into a table
    void CheckAndAddPredicateDep(const std::string &tab_name, Transaction *txn) {
        std::unique_lock<std::mutex> lock(latch_);
        for (auto &[tid, other] : txn_map) {
            if (other == txn) continue;
            if (other->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
            if (other->get_state() == TransactionState::COMMITTED ||
                other->get_state() == TransactionState::ABORTED) continue;
            auto pred = other->get_predicate_reads();
            if (pred->count(tab_name)) {
                other->add_pred_rw_out(txn->get_transaction_id());
                txn->add_pred_rw_in(other->get_transaction_id());
            }
        }
    }

    // SSI: global read set (RID -> set of txn_ids that read it)
    std::unordered_map<Rid, std::unordered_set<txn_id_t>> global_read_set_;
    std::mutex global_read_set_mutex_;

    // DELETE-INSERT conflict tracking: table_fd -> key -> set of deleting txn_ids
    struct DeleteKey {
        int fd;
        std::string key_data;
        bool operator==(const DeleteKey &o) const { return fd == o.fd && key_data == o.key_data; }
    };
    struct DeleteKeyHash {
        size_t operator()(const DeleteKey &k) const {
            return std::hash<int>()(k.fd) ^ std::hash<std::string>()(k.key_data);
        }
    };
    std::unordered_map<DeleteKey, std::unordered_set<txn_id_t>, DeleteKeyHash> pending_deletes_;
    std::mutex pending_deletes_mutex_;

    void AddPendingDelete(int fd, const std::string &key_data, txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(pending_deletes_mutex_);
        pending_deletes_[{fd, key_data}].insert(txn_id);
    }
    void RemovePendingDelete(int fd, const std::string &key_data, txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(pending_deletes_mutex_);
        auto it = pending_deletes_.find({fd, key_data});
        if (it != pending_deletes_.end()) {
            it->second.erase(txn_id);
            if (it->second.empty()) pending_deletes_.erase(it);
        }
    }
    bool HasPendingDelete(int fd, const std::string &key_data, txn_id_t my_txn_id) {
        std::lock_guard<std::mutex> lock(pending_deletes_mutex_);
        auto it = pending_deletes_.find({fd, key_data});
        if (it != pending_deletes_.end()) {
            for (auto tid : it->second) {
                if (tid != my_txn_id) {
                    auto *t = get_transaction_safe(tid);
                    if (t && t->get_state() != TransactionState::COMMITTED &&
                        t->get_state() != TransactionState::ABORTED)
                        return true;
                }
            }
        }
        return false;
    }

    void AddToGlobalReadSet(const Rid &rid, txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(global_read_set_mutex_);
        global_read_set_[rid].insert(txn_id);
    }

    void RemoveTxnFromGlobalReadSet(txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(global_read_set_mutex_);
        for (auto &[rid, readers] : global_read_set_) {
            readers.erase(txn_id);
        }
    }

    std::unordered_set<txn_id_t> GetReaders(const Rid &rid) {
        std::lock_guard<std::mutex> lock(global_read_set_mutex_);
        auto it = global_read_set_.find(rid);
        if (it != global_read_set_.end()) return it->second;
        return {};
    }

    // SSI: check for dangerous structure after adding rw dependency
    bool CheckSSIDangerous(Transaction *txn) {
        if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) return false;
        // Unified self-loop check: combine pred + write-path deps
        for (auto tin : txn->pred_rw_in_) {
            for (auto tout : txn->pred_rw_out_) {
                if (tin == tout) return true;
            }
            for (auto tout : txn->rw_out_) {
                if (tin == tout) return true;
            }
        }
        for (auto tin : txn->rw_in_) {
            for (auto tout : txn->pred_rw_out_) {
                if (tin == tout) return true;
            }
        }
        auto my_ws = txn->get_write_set();
        auto my_rs = txn->get_read_set();
        if (my_ws->empty() || my_rs->empty()) return false;
        std::unique_lock<std::mutex> lock(latch_);
        for (auto &[tid, other] : txn_map) {
            if (other == txn) continue;
            if (other->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
            if (other->get_state() == TransactionState::COMMITTED ||
                other->get_state() == TransactionState::ABORTED) continue;
            auto other_ws = other->get_write_set();
            auto other_rs = other->get_read_set();
            if (other_ws->empty() || other_rs->empty()) continue;
            // RID-level crossed deps
            bool i_read_other = false, other_read_me = false;
            for (auto &owr : *other_ws) if (my_rs->count(owr->GetRid())) { i_read_other = true; break; }
            for (auto &wr : *my_ws) if (other_rs->count(wr->GetRid())) { other_read_me = true; break; }
            if (i_read_other && other_read_me) return true;
            // Predicate-level phantom: I have predicate on other's write table,
            // OR other has predicate on my write table (combined with RID-level)
            auto my_pred = txn->get_predicate_reads();
            auto other_pred = other->get_predicate_reads();
            auto my_wt = txn->get_write_tables();
            auto other_wt = other->get_write_tables();
            bool i_pred_other = false, other_pred_me = false;
            for (auto &t : *other_wt) if (!my_pred->empty() && my_pred->count(t)) { i_pred_other = true; break; }
            for (auto &t : *my_wt) if (!other_pred->empty() && other_pred->count(t)) { other_pred_me = true; break; }
            // Phantom: I read (predicate) other's write AND other read (RID or predicate) my write
            if (i_pred_other && other_read_me) return true;
            if (i_read_other && other_pred_me) return true;
            if (i_pred_other && other_pred_me) return true;
        }
        return false;
    }

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();

    struct PageVersionInfo {
        std::shared_mutex mutex_;
        /** 存储所有槽的先前版本信息。注意：不要使用 `[x]` 来访问它，因为
         * 即使不存在也会创建新元素。请使用 `find` 来代替。
         */
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储表堆中每个元组的先前版本。 */
    std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> version_info_;


private:
    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};    // 用于分发事务时间戳
    std::mutex latch_;  // 用于txn_map的并发
    SmManager *sm_manager_;
    LockManager *lock_manager_;

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC
};