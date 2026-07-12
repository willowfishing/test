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
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"

struct UndoLink {
  txn_id_t prev_txn_{INVALID_TXN_ID};
  int prev_log_idx_{0};
  friend auto operator==(const UndoLink &a, const UndoLink &b) {
    return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
  }
  friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }
  bool IsValid() { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
  bool is_deleted_;
  std::vector<bool> modified_fields_;
  std::vector<Value> tuple_;
  RmRecord* tuple_test_ = nullptr;
  timestamp_t ts_{INVALID_TS};
  UndoLink prev_version_{};
};


class Transaction {
   public:
    using SnapshotRecords = std::vector<std::pair<Rid, RmRecord>>;

    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *>>();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId>>();
        index_latch_page_set_ = std::make_shared<std::deque<Page *>>();
        index_deleted_page_set_ = std::make_shared<std::deque<Page*>>();
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
    inline void set_isolation_level(IsolationLevel level) { isolation_level_ = level; }
    inline TransactionState get_state() { return state_; }
    inline void set_state(TransactionState state) { state_ = state; }
    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }
    inline std::shared_ptr<std::deque<WriteRecord *>> get_write_set() { return write_set_; }
    inline void append_write_record(WriteRecord* write_record) { write_set_->push_back(write_record); }
    inline std::shared_ptr<std::deque<Page*>> get_index_deleted_page_set() { return index_deleted_page_set_; }
    inline void append_index_deleted_page(Page* page) { index_deleted_page_set_->push_back(page); }
    inline std::shared_ptr<std::deque<Page*>> get_index_latch_page_set() { return index_latch_page_set_; }
    inline void append_index_latch_page_set(Page* page) { index_latch_page_set_->push_back(page); }
    inline std::shared_ptr<std::unordered_set<LockDataId>> get_lock_set() { return lock_set_; }
    inline void clear_snapshot() { snapshots_.clear(); }

    // ===== SSI methods =====
    inline void add_read_record(const std::string &tab_name, const Rid &rid) {
        read_set_[tab_name].insert(rid);
    }
    inline void add_predicate_read(const std::string &tab_name, const std::vector<Condition> &conds) {
        predicate_reads_.emplace_back(tab_name, conds);
    }
    inline const auto &get_read_set() const { return read_set_; }
    inline const auto &get_predicate_reads() const { return predicate_reads_; }
    inline void add_rw_dependency_out(txn_id_t writer_txn) { rw_out_.push_back(writer_txn); }
    inline void add_rw_dependency_in(txn_id_t reader_txn) { rw_in_.push_back(reader_txn); }
    inline const auto &get_rw_out() const { return rw_out_; }
    inline const auto &get_rw_in() const { return rw_in_; }
    inline void clear_ssi_state() {
        read_set_.clear();
        predicate_reads_.clear();
        rw_out_.clear();
        rw_in_.clear();
    }

    inline void set_snapshot_records(const std::string &tab_name, SnapshotRecords records) {
        snapshots_[tab_name] = std::move(records);
    }
    inline bool has_snapshot(const std::string &tab_name) const {
        return snapshots_.find(tab_name) != snapshots_.end();
    }
    inline const SnapshotRecords *get_snapshot_records(const std::string &tab_name) const {
        auto iter = snapshots_.find(tab_name);
        if (iter == snapshots_.end()) return nullptr;
        return &iter->second;
    }
    inline void remove_snapshot_record(const std::string &tab_name, const Rid &rid) {
        auto iter = snapshots_.find(tab_name);
        if (iter == snapshots_.end()) return;
        auto &records = iter->second;
        records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &entry) {
            return entry.first == rid;
        }), records.end());
    }
    inline void upsert_snapshot_record(const std::string &tab_name, const Rid &rid, const RmRecord &record) {
        auto iter = snapshots_.find(tab_name);
        if (iter == snapshots_.end()) return;
        remove_snapshot_record(tab_name, rid);
        snapshots_[tab_name].emplace_back(rid, record);
    }

    // TIMESTAMP accessors
    inline timestamp_t get_read_ts() const { return read_ts_; }
    inline void set_read_ts(timestamp_t ts) { read_ts_.store(ts); }
    inline timestamp_t get_commit_ts() const { return commit_ts_.load(); }
    inline void set_commit_ts(timestamp_t ts) { commit_ts_.store(ts); }

    // UNDO LOG methods
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
    }
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
    }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
    }
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
    }

   private:
    bool txn_mode_;
    TransactionState state_;
    IsolationLevel isolation_level_;
    std::thread::id thread_id_;
    lsn_t prev_lsn_;
    txn_id_t txn_id_;
    timestamp_t start_ts_;

    std::shared_ptr<std::deque<WriteRecord *>> write_set_;
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set_;
    std::shared_ptr<std::deque<Page*>> index_latch_page_set_;
    std::shared_ptr<std::deque<Page*>> index_deleted_page_set_;

    std::atomic<timestamp_t> read_ts_{0};
    std::atomic<timestamp_t> commit_ts_{INVALID_TS};
    std::unordered_map<std::string, SnapshotRecords> snapshots_;
    std::vector<UndoLog> undo_logs_;
    std::mutex latch_;

    // SSI tracking
    std::unordered_map<std::string, std::unordered_set<Rid>> read_set_;
    std::vector<std::pair<std::string, std::vector<Condition>>> predicate_reads_;
    std::vector<txn_id_t> rw_out_;
    std::vector<txn_id_t> rw_in_;
};
