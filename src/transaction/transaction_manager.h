/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "system/sm_manager.h"
#include "transaction.h"
#include "watermark.h"

/* 系统采用的并发控制算法。 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };

struct TxnUniqueKey {
    std::string table;
    std::string index_id;
    std::string encoded_key;

    friend bool operator==(const TxnUniqueKey &lhs, const TxnUniqueKey &rhs) {
        return lhs.table == rhs.table && lhs.index_id == rhs.index_id &&
               lhs.encoded_key == rhs.encoded_key;
    }
};

struct TxnUniqueKeyHash {
    size_t operator()(const TxnUniqueKey &key) const {
        const size_t h1 = std::hash<std::string>{}(key.table);
        const size_t h2 = std::hash<std::string>{}(key.index_id);
        const size_t h3 = std::hash<std::string>{}(key.encoded_key);
        return h1 ^ (h2 << 1U) ^ (h3 << 7U);
    }
};

struct UniqueKeyCommitState {
    timestamp_t commit_ts{0};
    // true: the latest committed database image contains this key;
    // false: the latest committed operation removed this key.
    bool present{false};
};

// A table without an index has no durable logical-key identity that can be
// used to connect a DELETE of an old snapshot row with a concurrent INSERT of
// the same row value.  Keep a narrowly-scoped value key for that case.  It is
// deliberately used only for DELETE -> INSERT conflict detection, so normal
// duplicate INSERTs into a heap table remain legal.
struct TxnDeletedValueKey {
    std::string table;
    std::string encoded_record;

    friend bool operator==(const TxnDeletedValueKey &lhs,
                           const TxnDeletedValueKey &rhs) {
        return lhs.table == rhs.table &&
               lhs.encoded_record == rhs.encoded_record;
    }
};

struct TxnDeletedValueKeyHash {
    size_t operator()(const TxnDeletedValueKey &key) const {
        const size_t h1 = std::hash<std::string>{}(key.table);
        const size_t h2 = std::hash<std::string>{}(key.encoded_record);
        return h1 ^ (h2 << 1U);
    }
};

struct VersionUndoLink {
    UndoLink prev_;
    bool in_progress_{false};
    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }
    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }
    static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) return VersionUndoLink{*undo_link};
        return std::nullopt;
    }
};

class TransactionManager {
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                                ConcurrencyMode concurrency_mode = ConcurrencyMode::MVCC)
        : concurrency_mode_(concurrency_mode), sm_manager_(sm_manager),
          lock_manager_(lock_manager) {}

    ~TransactionManager() = default;

    Transaction *begin(Transaction *txn, LogManager *log_manager,
                       IsolationLevel level = IsolationLevel::SERIALIZABLE);
    void start_explicit(Transaction *txn);
    void commit(Transaction *txn, LogManager *log_manager);
    void abort(Transaction *txn, LogManager *log_manager);

    ConcurrencyMode get_concurrency_mode() const { return concurrency_mode_; }
    void set_concurrency_mode(ConcurrencyMode mode) { concurrency_mode_ = mode; }
    LockManager *get_lock_manager() { return lock_manager_; }

    Transaction *get_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) return nullptr;
        std::lock_guard<std::mutex> lock(latch_);
        auto it = txn_map.find(txn_id);
        return it == txn_map.end() ? nullptr : it->second;
    }

    std::vector<SnapshotRow> get_visible_rows(const std::string &table,
                                              Transaction *txn);
    std::shared_ptr<RmRecord> get_visible_record(const std::string &table,
                                                 const Rid &rid,
                                                 Transaction *txn);

    Rid register_insert(const std::string &table, const RmRecord &record,
                        Transaction *txn);
    void register_update(const std::string &table, const Rid &rid,
                         const RmRecord &before, const RmRecord &after,
                         Transaction *txn);
    void register_delete(const std::string &table, const Rid &rid,
                         const RmRecord &before, Transaction *txn);

    void validate_unique_batch(
        const std::string &table,
        const std::vector<std::pair<Rid, std::shared_ptr<RmRecord>>> &candidates,
        const std::unordered_set<TxnRowKey, TxnRowKeyHash> &affected,
        Transaction *txn);

    void register_predicate_read(Transaction *txn, const std::string &table,
                                 const std::vector<Condition> &conditions,
                                 const std::vector<ColMeta> &columns);
    void register_record_read(Transaction *txn, const std::string &table,
                              const Rid &rid);

    // Compatibility hook for recovery/index rebuild. The current main branch
    // physically removes deleted rows, so this always returns false.
    bool is_committed_deleted(const std::string &table, const Rid &rid) const;

    std::vector<CheckpointTombstone> snapshot_tombstones() const;
    void restore_tombstones(const std::vector<CheckpointTombstone> &tombstones);
    void set_recovered_tombstone(const std::string &table, const Rid &rid,
                                 bool deleted);
    std::vector<txn_id_t> active_transaction_ids(
        txn_id_t exclude = INVALID_TXN_ID) const;
    void abort_all_active(LogManager *log_manager,
                          txn_id_t exclude = INVALID_TXN_ID);
    void set_next_txn_id(txn_id_t next_txn_id);
    void reclaim_retired_transactions();

    static std::unordered_map<txn_id_t, Transaction *> txn_map;
    std::shared_mutex txn_map_mutex_;

    // Framework-compatible MVCC helper declarations retained for source compatibility.
    bool UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);
    bool UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);
    std::optional<UndoLink> GetUndoLink(Rid rid);
    std::optional<VersionUndoLink> GetVersionLink(Rid rid);
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);
    UndoLog GetUndoLog(UndoLink link);
    timestamp_t GetWatermark();
    void GarbageCollection();

    struct PageVersionInfo {
        std::shared_mutex mutex_;
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };
    std::shared_mutex version_info_mutex_;
    std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> version_info_;

private:
    bool acquire_write_owner(const TxnRowKey &key, Transaction *txn);
    void release_write_owners(Transaction *txn);
    void add_rw_dependency(Transaction *reader, Transaction *writer,
                           Transaction *current_statement_txn);
    bool has_dangerous_structure() const;
    bool transactions_overlap(const Transaction *a, const Transaction *b) const;
    bool write_event_matches_predicate(const MvccWriteEvent &event,
                                       const PredicateRead &predicate) const;
    bool record_matches(const RmRecord &record,
                        const std::vector<Condition> &conditions,
                        const std::vector<PredicateColumn> &columns) const;
    void compare_new_write_with_readers(Transaction *writer,
                                        const MvccWriteEvent &event);
    void compare_new_read_with_writers(Transaction *reader,
                                       const PredicateRead *predicate,
                                       const TxnRowKey *record_key);
    void cleanup_aborted_dependencies(Transaction *txn);
    std::vector<TxnUniqueKey> unique_keys_for_record(
        const std::string &table, const RmRecord &record) const;
    void check_unique_key_owner_conflict(const TxnUniqueKey &key,
                                         Transaction *txn);
    void check_unique_key_version_conflict(const TxnUniqueKey &key,
                                           Transaction *txn);
    void reserve_unique_keys(const std::string &table, const RmRecord &record,
                             Transaction *txn, bool check_committed_version);
    TxnDeletedValueKey deleted_value_key(const std::string &table,
                                         const RmRecord &record) const;
    void check_deleted_value_insert_conflict(const std::string &table,
                                             const RmRecord &record,
                                             Transaction *txn);
    void reserve_deleted_value(const std::string &table,
                               const RmRecord &record,
                               Transaction *txn);
    void update_deleted_value_commit_states(Transaction *txn,
                                            timestamp_t commit_ts);
    bool committed_unique_key_exists(const TxnUniqueKey &key) const;
    void update_unique_commit_states(Transaction *txn, timestamp_t commit_ts);
    void apply_commit(Transaction *txn, timestamp_t commit_ts,
                      LogManager *log_manager);
    void prune_finished_transactions();

    ConcurrencyMode concurrency_mode_;
    std::atomic<txn_id_t> next_txn_id_{0};
    std::atomic<timestamp_t> next_timestamp_{1};
    mutable std::mutex latch_;
    SmManager *sm_manager_;
    LockManager *lock_manager_;

    std::atomic<timestamp_t> last_commit_ts_{0};
    Watermark running_txns_{0};

    mutable std::recursive_mutex mvcc_latch_;
    std::unordered_map<TxnRowKey, txn_id_t, TxnRowKeyHash> write_owners_;
    std::unordered_map<TxnRowKey, timestamp_t, TxnRowKeyHash> latest_commit_ts_;
    std::unordered_map<TxnUniqueKey, txn_id_t, TxnUniqueKeyHash> unique_write_owners_;
    std::unordered_map<TxnUniqueKey, UniqueKeyCommitState, TxnUniqueKeyHash>
        latest_unique_commit_states_;
    std::unordered_map<TxnDeletedValueKey, txn_id_t, TxnDeletedValueKeyHash>
        deleted_value_owners_;
    std::unordered_map<TxnDeletedValueKey, timestamp_t,
                       TxnDeletedValueKeyHash>
        latest_deleted_value_commit_ts_;
    std::unordered_set<txn_id_t> active_txn_ids_;
    std::vector<Transaction *> retired_txns_;
};
