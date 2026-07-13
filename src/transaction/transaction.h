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

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_set>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

/** 表示此tuple的前一个版本的链接 */
struct UndoLink {
  /* 之前的版本可以在其中的事务中找到 */
  txn_id_t prev_txn_{INVALID_TXN_ID};
  /* 在 `prev_txn_` 中前一个版本的日志索引 */
  int prev_log_idx_{0};

  friend auto operator==(const UndoLink &a, const UndoLink &b) {
    return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
  }

  friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }

  /* Checks if the undo link points to something. */
  bool IsValid() const { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
  /* 此日志是否为删除标记 */
  bool is_deleted_{false};
  /* 此撤销日志修改的字段 */
  std::vector<bool> modified_fields_;
  /* 修改后的字段 */
  std::vector<Value> tuple_;
  std::shared_ptr<RmRecord> tuple_test_{nullptr};
  /* 此撤销日志的时间戳 */
  timestamp_t ts_{INVALID_TS};
  /* 撤销日志的前一个版本 */
  UndoLink prev_version_{};
};


class Transaction {
   public:
    struct PredicateRead {
        std::string tab_name;
        std::vector<ColMeta> cols;
        std::vector<Condition> conds;
    };
    struct SerializableWriteInfo {
        std::string tab_name;
        Rid rid;
        std::vector<ColMeta> cols;
        std::shared_ptr<RmRecord> old_record;
        std::shared_ptr<RmRecord> new_record;
    };
    struct ReadColumnValue {
        ColType type;
        int len;
        std::string data;
    };

    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() { return txn_id_; }

    inline std::thread::id get_thread_id() { return thread_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() { return txn_mode_; }

    inline void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; }
    inline timestamp_t get_start_ts() { return start_ts_; }

    inline IsolationLevel get_isolation_level() { return isolation_level_; }
    inline void set_isolation_level(IsolationLevel isolation_level) { isolation_level_ = isolation_level; }

    inline bool watermark_registered() const { return watermark_registered_; }
    inline void set_watermark_registered(bool registered) { watermark_registered_ = registered; }

    inline bool gc_ready() const { return gc_ready_; }
    inline void set_gc_ready(bool ready) { gc_ready_ = ready; }

    inline bool admission_active() const { return admission_active_; }
    inline void set_admission_active(bool active) { admission_active_ = active; }

    inline bool ssi_metadata_released() const {
        return ssi_metadata_released_.load(std::memory_order_acquire);
    }
    inline void set_ssi_metadata_released(bool released) {
        ssi_metadata_released_.store(released, std::memory_order_release);
    }

    inline TransactionState get_state() { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }

    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    inline std::vector<WriteRecord> &get_write_set() { return write_set_; }
    inline const std::vector<WriteRecord> &get_write_set() const { return write_set_; }

    template <typename... Args>
    inline void emplace_write_record(Args &&...args) {
        write_set_.emplace_back(std::forward<Args>(args)...);
    }

    inline const std::vector<lock_data_key_t> &get_lock_set() const { return lock_set_; }

    inline void add_lock(lock_data_key_t lock_key) {
        lock_set_.push_back(lock_key);
    }

    inline void remove_lock(lock_data_key_t lock_key) {
        lock_set_.erase(std::remove(lock_set_.begin(), lock_set_.end(), lock_key), lock_set_.end());
    }

    inline void clear_locks() { lock_set_.clear(); }

    inline void add_read_record(const std::string &tab_name, Rid rid) {
        read_records_[tab_name].push_back(rid);
    }

    inline const std::unordered_map<std::string, std::vector<Rid>> &get_read_records() const {
        return read_records_;
    }

    inline void add_predicate_read(std::string tab_name, std::vector<ColMeta> cols,
                                   std::vector<Condition> conds) {
        predicate_reads_.push_back({std::move(tab_name), std::move(cols), std::move(conds)});
    }

    inline const std::vector<PredicateRead> &get_predicate_reads() const {
        return predicate_reads_;
    }

    inline void upsert_serializable_write(std::string tab_name, Rid rid, std::vector<ColMeta> cols,
                                          std::shared_ptr<RmRecord> old_record,
                                          std::shared_ptr<RmRecord> new_record) {
        serializable_writes_[serializable_write_key(tab_name, rid)] = {
            std::move(tab_name), rid, std::move(cols), std::move(old_record), std::move(new_record)};
    }

    inline const std::unordered_map<std::string, SerializableWriteInfo> &get_serializable_writes() const {
        return serializable_writes_;
    }

    inline void add_rw_dependency(txn_id_t txn_id) {
        rw_dependencies_.insert(txn_id);
    }

    inline const std::unordered_set<txn_id_t> &get_rw_dependencies() const {
        return rw_dependencies_;
    }

    inline void remove_rw_dependency(txn_id_t txn_id) {
        rw_dependencies_.erase(txn_id);
    }

    inline void clear_serializable_state() {
        read_records_.clear();
        predicate_reads_.clear();
        rw_dependencies_.clear();
        serializable_writes_.clear();
        read_column_values_.clear();
    }

    inline void remember_read_column(const std::string &tab_name, const Rid &rid, const ColMeta &col,
                                     const char *value) {
        if (value == nullptr || col.name.empty() || col.len <= 0) {
            return;
        }
        ReadColumnValue stored{col.type, col.len, std::string(value, static_cast<size_t>(col.len))};
        read_column_values_[serializable_write_key(tab_name, rid)][col.name] = std::move(stored);
    }

    inline bool get_read_column_value(const std::string &tab_name, const Rid &rid, const std::string &col_name,
                                      ReadColumnValue *out) const {
        auto rec_iter = read_column_values_.find(serializable_write_key(tab_name, rid));
        if (rec_iter == read_column_values_.end()) {
            return false;
        }
        auto col_iter = rec_iter->second.find(col_name);
        if (col_iter == rec_iter->second.end()) {
            return false;
        }
        if (out != nullptr) {
            *out = col_iter->second;
        }
        return true;
    }

    inline timestamp_t get_read_ts() const { return read_ts_; }
    inline void set_read_ts(timestamp_t read_ts) { read_ts_ = read_ts; }
    inline timestamp_t get_commit_ts() const { return commit_ts_; }
    inline void set_commit_ts(timestamp_t commit_ts) { commit_ts_ = commit_ts; }

    inline void clear_write_records() {
        write_set_.clear();
    }

    /** 修改现有的撤销日志 */
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
      }

    /** @return 此事务中撤销日志的索引 */
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
      }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
      }

    /** @return 撤销日志的数量 */
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
      }

    inline std::vector<UndoLog> TakeUndoLogs() {
        std::scoped_lock<std::mutex> lck(latch_);
        std::vector<UndoLog> logs;
        logs.swap(undo_logs_);
        return logs;
    }

    inline void RetainUndoReference() {
        undo_ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    inline void ReleaseUndoReference() {
        int64_t previous = undo_ref_count_.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
    }

    inline int64_t GetUndoReferenceCount() const {
        return undo_ref_count_.load(std::memory_order_acquire);
    }


    using SnapshotRecords = std::vector<std::pair<Rid, RmRecord>>;
    std::unordered_map<std::string, SnapshotRecords> snapshots_;
    inline void clear_snapshot() { snapshots_.clear(); }
    inline const SnapshotRecords *get_snapshot_records(const std::string &tab) const { auto it = snapshots_.find(tab); return it != snapshots_.end() ? &it->second : nullptr; }
    inline void set_snapshot_records(const std::string &tab, SnapshotRecords rec) { snapshots_[tab] = std::move(rec); }
   private:
    static std::string serializable_write_key(const std::string &tab_name, const Rid &rid) {
        return tab_name + "#" + std::to_string(rid.page_no) + ":" + std::to_string(rid.slot_no);
    }

    bool txn_mode_;                   // 用于标识当前事务为显式事务还是单条SQL语句的隐式事务
    TransactionState state_;          // 事务状态
    IsolationLevel isolation_level_;  // 事务的隔离级别，默认隔离级别为可串行化
    bool watermark_registered_{false};
    bool gc_ready_{false};
    bool admission_active_{false};
    std::atomic<bool> ssi_metadata_released_{true};
    std::thread::id thread_id_;       // 当前事务对应的线程id
    lsn_t prev_lsn_;                  // 当前事务执行的最后一条操作对应的lsn，用于系统故障恢复
    txn_id_t txn_id_;                 // 事务的ID，唯一标识符
    timestamp_t start_ts_;            // 事务的开始时间戳

    std::vector<WriteRecord> write_set_;  // 事务包含的所有写操作，首次写入时才分配
    std::vector<lock_data_key_t> lock_set_;  // 事务申请的所有锁，首次加锁时才分配

  std::atomic<timestamp_t> read_ts_{0};
  /** 提交时间戳 */
  std::atomic<timestamp_t> commit_ts_{INVALID_TS};
  std::unordered_map<std::string, std::vector<Rid>> read_records_;
  std::unordered_map<std::string, std::unordered_map<std::string, ReadColumnValue>> read_column_values_;
  std::vector<PredicateRead> predicate_reads_;
  std::unordered_set<txn_id_t> rw_dependencies_;
  std::unordered_map<std::string, SerializableWriteInfo> serializable_writes_;
  /** Number of page-head and cross-transaction links that still reach this transaction's undo logs. */
  std::atomic<int64_t> undo_ref_count_{0};
  /**
  * @brief 存储撤销日志。
  * 其他撤销日志/表堆将存储 (txn_id, index) 对，因此只能向此vector中追加内容或就地更新内容，而不能删除任何内容。
  */
  std::vector<UndoLog> undo_logs_;
  /** 用于访问事务级撤销日志的锁。 */
  std::mutex latch_;
};
