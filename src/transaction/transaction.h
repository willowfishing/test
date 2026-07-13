/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common.h"
#include "record/rm_defs.h"
#include "transaction/txn_defs.h"

/** 表示此 tuple 的前一个版本的链接（保留框架原接口）。 */
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
    RmRecord *tuple_test_;
    timestamp_t ts_{INVALID_TS};
    UndoLink prev_version_{};
};

struct TxnRowKey {
    std::string table;
    Rid rid{};

    friend bool operator==(const TxnRowKey &lhs, const TxnRowKey &rhs) {
        return lhs.table == rhs.table && lhs.rid == rhs.rid;
    }
};

struct TxnRowKeyHash {
    size_t operator()(const TxnRowKey &key) const {
        const size_t h1 = std::hash<std::string>{}(key.table);
        const size_t h2 = std::hash<int>{}(key.rid.page_no);
        const size_t h3 = std::hash<int>{}(key.rid.slot_no);
        return h1 ^ (h2 << 1U) ^ (h3 << 7U);
    }
};

struct SnapshotRow {
    Rid rid{};
    std::shared_ptr<RmRecord> record;
};

struct PendingWrite {
    bool inserted{false};
    bool deleted{false};
    std::shared_ptr<RmRecord> before;
    std::shared_ptr<RmRecord> current;
};

struct PredicateColumn {
    std::string tab_name;
    std::string name;
    ColType type{};
    int len{0};
    int offset{0};
};

struct PredicateRead {
    std::string table;
    std::vector<Condition> conditions;
    std::vector<PredicateColumn> columns;
};

struct MvccWriteEvent {
    TxnRowKey key;
    bool has_before{false};
    bool has_after{false};
    std::shared_ptr<RmRecord> before;
    std::shared_ptr<RmRecord> after;
};

class Transaction {
public:
    explicit Transaction(txn_id_t txn_id,
                         IsolationLevel isolation_level = IsolationLevel::SERIALIZABLE)
        : txn_mode_(false), state_(TransactionState::DEFAULT),
          isolation_level_(isolation_level), txn_id_(txn_id) {
        write_set_ = std::make_shared<std::deque<WriteRecord *>>();
        lock_set_ = std::make_shared<std::unordered_set<LockDataId>>();
        index_latch_page_set_ = std::make_shared<std::deque<Page *>>();
        index_deleted_page_set_ = std::make_shared<std::deque<Page *>>();
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() { clear_write_set(); }

    txn_id_t get_transaction_id() const { return txn_id_; }
    std::thread::id get_thread_id() const { return thread_id_; }

    void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    bool get_txn_mode() const { return txn_mode_; }

    void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; read_ts_ = start_ts; }
    timestamp_t get_start_ts() const { return start_ts_; }

    IsolationLevel get_isolation_level() const { return isolation_level_; }
    void set_isolation_level(IsolationLevel level) { isolation_level_ = level; }

    TransactionState get_state() const { return state_; }
    void set_state(TransactionState state) { state_ = state; }

    lsn_t get_prev_lsn() const { return prev_lsn_; }
    void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    std::shared_ptr<std::deque<WriteRecord *>> get_write_set() { return write_set_; }
    void append_write_record(WriteRecord *write_record) { write_set_->push_back(write_record); }
    void clear_write_set() {
        while (!write_set_->empty()) {
            delete write_set_->back();
            write_set_->pop_back();
        }
    }

    std::shared_ptr<std::deque<Page *>> get_index_deleted_page_set() { return index_deleted_page_set_; }
    void append_index_deleted_page(Page *page) { index_deleted_page_set_->push_back(page); }
    std::shared_ptr<std::deque<Page *>> get_index_latch_page_set() { return index_latch_page_set_; }
    void append_index_latch_page_set(Page *page) { index_latch_page_set_->push_back(page); }
    std::shared_ptr<std::unordered_set<LockDataId>> get_lock_set() { return lock_set_; }

    timestamp_t get_read_ts() const { return read_ts_; }
    timestamp_t get_commit_ts() const { return commit_ts_; }
    void set_commit_ts(timestamp_t ts) { commit_ts_ = ts; }

    void set_snapshot(std::unordered_map<std::string, std::vector<SnapshotRow>> snapshot) {
        snapshot_ = std::move(snapshot);
        snapshot_ready_ = true;
    }
    bool has_snapshot() const { return snapshot_ready_; }
    const std::vector<SnapshotRow> *get_snapshot_rows(const std::string &table) const {
        auto it = snapshot_.find(table);
        return it == snapshot_.end() ? nullptr : &it->second;
    }

    std::unordered_map<TxnRowKey, PendingWrite, TxnRowKeyHash> &pending_writes() {
        return pending_writes_;
    }
    const std::unordered_map<TxnRowKey, PendingWrite, TxnRowKeyHash> &pending_writes() const {
        return pending_writes_;
    }

    void remember_pending_key(const TxnRowKey &key) {
        if (pending_writes_.find(key) == pending_writes_.end()) pending_order_.push_back(key);
    }
    const std::vector<TxnRowKey> &pending_order() const { return pending_order_; }

    Rid allocate_temp_rid() {
        return Rid{-static_cast<int>(txn_id_) - 2, next_temp_slot_--};
    }

    void add_record_read(const TxnRowKey &key) { record_reads_.insert(key); }
    const std::unordered_set<TxnRowKey, TxnRowKeyHash> &record_reads() const { return record_reads_; }

    void add_predicate_read(PredicateRead read) { predicate_reads_.push_back(std::move(read)); }
    const std::vector<PredicateRead> &predicate_reads() const { return predicate_reads_; }

    void add_write_event(MvccWriteEvent event) { write_events_.push_back(std::move(event)); }
    const std::vector<MvccWriteEvent> &write_events() const { return write_events_; }

    void add_rw_out(txn_id_t to) { rw_out_.insert(to); }
    void add_rw_in(txn_id_t from) { rw_in_.insert(from); }
    void remove_rw_out(txn_id_t to) { rw_out_.erase(to); }
    void remove_rw_in(txn_id_t from) { rw_in_.erase(from); }
    const std::unordered_set<txn_id_t> &rw_out() const { return rw_out_; }
    const std::unordered_set<txn_id_t> &rw_in() const { return rw_in_; }

    void clear_mvcc_runtime() {
        pending_writes_.clear();
        pending_order_.clear();
        record_reads_.clear();
        predicate_reads_.clear();
        write_events_.clear();
        snapshot_.clear();
        snapshot_ready_ = false;
    }

    // Framework-compatible undo-log helpers.
    void ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
    }
    UndoLink AppendUndoLog(UndoLog log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
    }
    UndoLog GetUndoLog(size_t log_id) {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
    }
    size_t GetUndoLogNum() {
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
    timestamp_t start_ts_{0};

    std::shared_ptr<std::deque<WriteRecord *>> write_set_;
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set_;
    std::shared_ptr<std::deque<Page *>> index_latch_page_set_;
    std::shared_ptr<std::deque<Page *>> index_deleted_page_set_;

    std::atomic<timestamp_t> read_ts_{0};
    std::atomic<timestamp_t> commit_ts_{INVALID_TS};
    std::vector<UndoLog> undo_logs_;
    std::mutex latch_;

    bool snapshot_ready_{false};
    std::unordered_map<std::string, std::vector<SnapshotRow>> snapshot_;
    std::unordered_map<TxnRowKey, PendingWrite, TxnRowKeyHash> pending_writes_;
    std::vector<TxnRowKey> pending_order_;
    int next_temp_slot_{-1};

    std::unordered_set<TxnRowKey, TxnRowKeyHash> record_reads_;
    std::vector<PredicateRead> predicate_reads_;
    std::vector<MvccWriteEvent> write_events_;
    std::unordered_set<txn_id_t> rw_out_;
    std::unordered_set<txn_id_t> rw_in_;
};
