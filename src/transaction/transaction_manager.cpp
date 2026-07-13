/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "transaction_manager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "execution/index_key_utils.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

void release_transaction_locks(Transaction *txn, LockManager *lock_manager) {
    if (txn == nullptr || lock_manager == nullptr) return;
    auto lock_set = txn->get_lock_set();
    while (!lock_set->empty()) {
        auto it = lock_set->begin();
        const LockDataId lock_id = *it;
        lock_manager->unlock(txn, lock_id);
        lock_set->erase(it);
    }
}

int compare_bytes(const char *lhs, ColType lhs_type, int lhs_len,
                  const char *rhs, ColType rhs_type, int rhs_len) {
    if (lhs_type == TYPE_INT && rhs_type == TYPE_INT) {
        const int a = *reinterpret_cast<const int *>(lhs);
        const int b = *reinterpret_cast<const int *>(rhs);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (lhs_type == TYPE_FLOAT && rhs_type == TYPE_FLOAT) {
        const float a = *reinterpret_cast<const float *>(lhs);
        const float b = *reinterpret_cast<const float *>(rhs);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) {
        const float a = static_cast<float>(*reinterpret_cast<const int *>(lhs));
        const float b = *reinterpret_cast<const float *>(rhs);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) {
        const float a = *reinterpret_cast<const float *>(lhs);
        const float b = static_cast<float>(*reinterpret_cast<const int *>(rhs));
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    size_t a_len = 0;
    size_t b_len = 0;
    while (a_len < static_cast<size_t>(lhs_len) && lhs[a_len] != '\0') ++a_len;
    while (b_len < static_cast<size_t>(rhs_len) && rhs[b_len] != '\0') ++b_len;
    const size_t common = std::min(a_len, b_len);
    const int prefix = common == 0 ? 0 : std::memcmp(lhs, rhs, common);
    if (prefix != 0) return prefix;
    return a_len < b_len ? -1 : (a_len > b_len ? 1 : 0);
}

bool compare_result(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

std::string encoded_key(const IndexMeta &index, const RmRecord &record) {
    const auto key = make_index_key(index, record.data);
    return std::string(key.data(), key.size());
}

std::string index_identity(const IndexMeta &index) {
    std::string identity;
    for (const auto &column : index.cols) {
        identity.append(column.name);
        identity.push_back('\x1f');
        identity.append(std::to_string(static_cast<int>(column.type)));
        identity.push_back(':');
        identity.append(std::to_string(column.len));
        identity.push_back('\x1e');
    }
    return identity;
}

}  // namespace

void TransactionManager::prune_finished_transactions() {
    // Callers hold mvcc_latch_. Keep txn_map access synchronized with
    // get_transaction(), and retain committed SSI metadata only while an older
    // active snapshot may still reference it.
    std::lock_guard<std::mutex> map_guard(latch_);
    timestamp_t oldest_active_start = std::numeric_limits<timestamp_t>::max();
    for (txn_id_t txn_id : active_txn_ids_) {
        auto it = txn_map.find(txn_id);
        if (it != txn_map.end() && it->second != nullptr) {
            oldest_active_start = std::min(oldest_active_start, it->second->get_start_ts());
        }
    }

    for (auto it = txn_map.begin(); it != txn_map.end();) {
        Transaction *candidate = it->second;
        if (candidate == nullptr || active_txn_ids_.count(it->first) != 0) {
            ++it;
            continue;
        }
        const TransactionState state = candidate->get_state();
        bool safe = state == TransactionState::ABORTED;
        if (state == TransactionState::COMMITTED) {
            const timestamp_t commit_ts = candidate->get_commit_ts();
            safe = active_txn_ids_.empty() ||
                   (commit_ts != INVALID_TS && commit_ts < oldest_active_start);
        }
        if (!safe) {
            ++it;
            continue;
        }
        candidate->clear_mvcc_runtime();
        retired_txns_.push_back(candidate);
        it = txn_map.erase(it);
    }
}

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager,
                                       IsolationLevel level) {
    (void)log_manager;
    if (txn == nullptr) {
        std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
        prune_finished_transactions();
        const txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id, level);
        txn->set_state(TransactionState::DEFAULT);
        txn->set_start_ts(last_commit_ts_.load());
        {
            std::lock_guard<std::mutex> lock(latch_);
            txn_map[txn_id] = txn;
        }
        active_txn_ids_.insert(txn_id);
    }
    return txn;
}

void TransactionManager::start_explicit(Transaction *txn) {
    if (txn == nullptr) return;
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    txn->set_start_ts(last_commit_ts_.load());
    txn->set_state(TransactionState::GROWING);
    txn->set_txn_mode(true);

    std::unordered_map<std::string, std::vector<SnapshotRow>> snapshot;
    std::vector<std::string> tables;
    tables.reserve(sm_manager_->fhs_.size());
    for (const auto &[name, handle] : sm_manager_->fhs_) {
        (void)handle;
        tables.push_back(name);
    }
    std::sort(tables.begin(), tables.end());
    for (const auto &table : tables) {
        RmFileHandle *fh = sm_manager_->fhs_.at(table).get();
        std::vector<SnapshotRow> rows;
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            const Rid rid = scan.rid();
            rows.push_back({rid, std::make_shared<RmRecord>(*fh->get_record(rid, nullptr))});
        }
        snapshot.emplace(table, std::move(rows));
    }
    txn->set_snapshot(std::move(snapshot));
}

std::vector<SnapshotRow> TransactionManager::get_visible_rows(const std::string &table,
                                                              Transaction *txn) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    std::vector<SnapshotRow> base;
    if (txn != nullptr && txn->has_snapshot()) {
        if (const auto *rows = txn->get_snapshot_rows(table); rows != nullptr) base = *rows;
    } else {
        RmFileHandle *fh = sm_manager_->fhs_.at(table).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            const Rid rid = scan.rid();
            base.push_back({rid, std::make_shared<RmRecord>(*fh->get_record(rid, nullptr))});
        }
    }
    if (txn == nullptr) return base;

    std::vector<SnapshotRow> result;
    result.reserve(base.size() + txn->pending_writes().size());
    for (const auto &row : base) {
        const TxnRowKey key{table, row.rid};
        auto it = txn->pending_writes().find(key);
        if (it == txn->pending_writes().end()) {
            result.push_back(row);
        } else if (!it->second.deleted && it->second.current != nullptr) {
            result.push_back({row.rid, std::make_shared<RmRecord>(*it->second.current)});
        }
    }
    for (const auto &key : txn->pending_order()) {
        if (key.table != table) continue;
        auto it = txn->pending_writes().find(key);
        if (it == txn->pending_writes().end() || !it->second.inserted ||
            it->second.deleted || it->second.current == nullptr) {
            continue;
        }
        result.push_back({key.rid, std::make_shared<RmRecord>(*it->second.current)});
    }
    return result;
}

std::shared_ptr<RmRecord> TransactionManager::get_visible_record(const std::string &table,
                                                                 const Rid &rid,
                                                                 Transaction *txn) {
    auto rows = get_visible_rows(table, txn);
    for (const auto &row : rows) {
        if (row.rid == rid) return std::make_shared<RmRecord>(*row.record);
    }
    return nullptr;
}

bool TransactionManager::acquire_write_owner(const TxnRowKey &key, Transaction *txn) {
    auto owner = write_owners_.find(key);
    if (owner != write_owners_.end() && owner->second != txn->get_transaction_id()) {
        Transaction *other = nullptr;
        auto it = txn_map.find(owner->second);
        if (it != txn_map.end()) other = it->second;
        if (other != nullptr && other->get_state() != TransactionState::ABORTED &&
            other->get_state() != TransactionState::COMMITTED) {
            throw TransactionAbortException(txn->get_transaction_id(),
                                            AbortReason::WRITE_WRITE_CONFLICT);
        }
        write_owners_.erase(owner);
    }
    const auto latest = latest_commit_ts_.find(key);
    if (latest != latest_commit_ts_.end() && latest->second > txn->get_start_ts()) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::WRITE_WRITE_CONFLICT);
    }
    write_owners_[key] = txn->get_transaction_id();
    return true;
}

std::vector<TxnUniqueKey> TransactionManager::unique_keys_for_record(
    const std::string &table, const RmRecord &record) const {
    std::vector<TxnUniqueKey> result;
    const TabMeta &tab = sm_manager_->db_.get_table(table);
    result.reserve(tab.indexes.size());
    for (const auto &index : tab.indexes) {
        result.push_back({table, index_identity(index), encoded_key(index, record)});
    }
    return result;
}

void TransactionManager::check_unique_key_owner_conflict(
    const TxnUniqueKey &key, Transaction *txn) {
    auto owner = unique_write_owners_.find(key);
    if (owner == unique_write_owners_.end() ||
        owner->second == txn->get_transaction_id()) {
        return;
    }

    Transaction *other = nullptr;
    auto it = txn_map.find(owner->second);
    if (it != txn_map.end()) other = it->second;
    if (other != nullptr &&
        other->get_state() != TransactionState::ABORTED &&
        other->get_state() != TransactionState::COMMITTED) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::WRITE_WRITE_CONFLICT);
    }
    unique_write_owners_.erase(owner);
}

void TransactionManager::check_unique_key_version_conflict(
    const TxnUniqueKey &key, Transaction *txn) {
    check_unique_key_owner_conflict(key, txn);
    const auto latest = latest_unique_commit_states_.find(key);
    if (latest != latest_unique_commit_states_.end() &&
        latest->second.commit_ts > txn->get_start_ts()) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::WRITE_WRITE_CONFLICT);
    }
}

void TransactionManager::reserve_unique_keys(const std::string &table,
                                             const RmRecord &record,
                                             Transaction *txn,
                                             bool check_committed_version) {
    const auto keys = unique_keys_for_record(table, record);
    for (const auto &key : keys) {
        if (check_committed_version) {
            check_unique_key_version_conflict(key, txn);
        } else {
            check_unique_key_owner_conflict(key, txn);
        }
    }
    for (const auto &key : keys) {
        unique_write_owners_[key] = txn->get_transaction_id();
    }
}

TxnDeletedValueKey TransactionManager::deleted_value_key(
    const std::string &table, const RmRecord &record) const {
    return {table, std::string(record.data, record.size)};
}

void TransactionManager::check_deleted_value_insert_conflict(
    const std::string &table, const RmRecord &record, Transaction *txn) {
    // Indexed tables already use their index key(s) as logical identities.
    // The fallback below is needed only for heap tables with no index.
    if (!sm_manager_->db_.get_table(table).indexes.empty()) return;

    const TxnDeletedValueKey key = deleted_value_key(table, record);
    auto owner = deleted_value_owners_.find(key);
    if (owner != deleted_value_owners_.end() &&
        owner->second != txn->get_transaction_id()) {
        Transaction *other = nullptr;
        auto txn_it = txn_map.find(owner->second);
        if (txn_it != txn_map.end()) other = txn_it->second;
        if (other != nullptr &&
            other->get_state() != TransactionState::ABORTED &&
            other->get_state() != TransactionState::COMMITTED) {
            throw TransactionAbortException(
                txn->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }
        deleted_value_owners_.erase(owner);
    }

    const auto latest = latest_deleted_value_commit_ts_.find(key);
    if (latest != latest_deleted_value_commit_ts_.end() &&
        latest->second > txn->get_start_ts()) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::WRITE_WRITE_CONFLICT);
    }
}

void TransactionManager::reserve_deleted_value(
    const std::string &table, const RmRecord &record, Transaction *txn) {
    if (!sm_manager_->db_.get_table(table).indexes.empty()) return;

    const TxnDeletedValueKey key = deleted_value_key(table, record);
    auto owner = deleted_value_owners_.find(key);
    if (owner != deleted_value_owners_.end() &&
        owner->second != txn->get_transaction_id()) {
        Transaction *other = nullptr;
        auto txn_it = txn_map.find(owner->second);
        if (txn_it != txn_map.end()) other = txn_it->second;
        if (other != nullptr &&
            other->get_state() != TransactionState::ABORTED &&
            other->get_state() != TransactionState::COMMITTED) {
            throw TransactionAbortException(
                txn->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }
        deleted_value_owners_.erase(owner);
    }
    deleted_value_owners_[key] = txn->get_transaction_id();
}

Rid TransactionManager::register_insert(const std::string &table,
                                        const RmRecord &record,
                                        Transaction *txn) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    check_deleted_value_insert_conflict(table, record, txn);
    reserve_unique_keys(table, record, txn, false);
    const Rid rid = txn->allocate_temp_rid();
    const TxnRowKey key{table, rid};
    txn->remember_pending_key(key);
    PendingWrite pending;
    pending.inserted = true;
    pending.current = std::make_shared<RmRecord>(record);
    txn->pending_writes()[key] = pending;

    MvccWriteEvent event;
    event.key = key;
    event.has_after = true;
    event.after = std::make_shared<RmRecord>(record);
    txn->add_write_event(event);
    if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        compare_new_write_with_readers(txn, event);
    }
    return rid;
}

void TransactionManager::register_update(const std::string &table, const Rid &rid,
                                         const RmRecord &before,
                                         const RmRecord &after,
                                         Transaction *txn) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    const TxnRowKey key{table, rid};
    reserve_unique_keys(table, before, txn, true);
    reserve_unique_keys(table, after, txn, true);
    auto existing = txn->pending_writes().find(key);
    if (existing == txn->pending_writes().end()) {
        acquire_write_owner(key, txn);
        txn->remember_pending_key(key);
        PendingWrite pending;
        pending.before = std::make_shared<RmRecord>(before);
        pending.current = std::make_shared<RmRecord>(after);
        txn->pending_writes()[key] = pending;
    } else {
        existing->second.deleted = false;
        existing->second.current = std::make_shared<RmRecord>(after);
    }

    MvccWriteEvent event;
    event.key = key;
    event.has_before = true;
    event.has_after = true;
    event.before = std::make_shared<RmRecord>(before);
    event.after = std::make_shared<RmRecord>(after);
    txn->add_write_event(event);
    if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        compare_new_write_with_readers(txn, event);
    }
}

void TransactionManager::register_delete(const std::string &table, const Rid &rid,
                                         const RmRecord &before,
                                         Transaction *txn) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    const TxnRowKey key{table, rid};
    reserve_unique_keys(table, before, txn, true);
    auto existing = txn->pending_writes().find(key);
    if (existing == txn->pending_writes().end()) {
        acquire_write_owner(key, txn);
        txn->remember_pending_key(key);
        PendingWrite pending;
        pending.before = std::make_shared<RmRecord>(before);
        pending.current = std::make_shared<RmRecord>(before);
        pending.deleted = true;
        txn->pending_writes()[key] = pending;
    } else {
        existing->second.deleted = true;
    }
    reserve_deleted_value(table, before, txn);

    MvccWriteEvent event;
    event.key = key;
    event.has_before = true;
    event.before = std::make_shared<RmRecord>(before);
    txn->add_write_event(event);
    if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        compare_new_write_with_readers(txn, event);
    }
}

void TransactionManager::validate_unique_batch(
    const std::string &table,
    const std::vector<std::pair<Rid, std::shared_ptr<RmRecord>>> &candidates,
    const std::unordered_set<TxnRowKey, TxnRowKeyHash> &affected,
    Transaction *txn) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    const TabMeta &tab = sm_manager_->db_.get_table(table);
    if (tab.indexes.empty()) return;

    // Check active unique-key owners before the committed B+ tree. An active
    // DELETE still leaves its old index entry installed until commit, but a
    // concurrent INSERT must be classified as a write-write conflict.
    std::vector<TxnUniqueKey> candidate_keys;
    for (const auto &[rid, record] : candidates) {
        (void)rid;
        if (record == nullptr) continue;
        auto keys = unique_keys_for_record(table, *record);
        for (const auto &key : keys) check_unique_key_owner_conflict(key, txn);
        candidate_keys.insert(candidate_keys.end(), keys.begin(), keys.end());
    }

    // Validate only keys touched by this statement/transaction. The old
    // implementation materialized the entire table for every indexed write,
    // making bulk INSERT and UPDATE workloads O(N^2).
    for (const auto &index : tab.indexes) {
        std::unordered_map<std::string, TxnRowKey> desired_keys;
        auto add_desired = [&](const TxnRowKey &row_key,
                               const std::shared_ptr<RmRecord> &record) {
            if (record == nullptr) return;
            const std::string encoded = encoded_key(index, *record);
            auto [it, inserted] = desired_keys.emplace(encoded, row_key);
            if (!inserted && !(it->second == row_key)) {
                throw UniqueConstraintError(table);
            }
        };

        // Earlier statements in the same transaction have not reached the
        // shared index yet, but they still participate in uniqueness.
        for (const auto &[row_key, pending] : txn->pending_writes()) {
            if (row_key.table != table || affected.count(row_key) != 0 ||
                pending.deleted || pending.current == nullptr) {
                continue;
            }
            add_desired(row_key, pending.current);
        }
        for (const auto &[rid, record] : candidates) {
            add_desired({table, rid}, record);
        }

        const std::string index_name =
            sm_manager_->get_ix_manager()->get_index_name(table, index.cols);
        auto index_handle = sm_manager_->ihs_.find(index_name);
        if (index_handle == sm_manager_->ihs_.end()) {
            throw InternalError("Index metadata exists without an open index handle");
        }

        for (const auto &[encoded, desired_row] : desired_keys) {
            std::vector<Rid> result;
            if (!index_handle->second->get_value(encoded.data(), &result, txn) ||
                result.empty()) {
                continue;
            }
            const TxnRowKey existing{table, result.front()};
            if (existing == desired_row || affected.count(existing) != 0) {
                continue;
            }

            // A previous statement in this transaction may already delete the
            // indexed row or move it to a different key. Its committed index
            // entry is removed only during commit and must not conflict here.
            auto pending = txn->pending_writes().find(existing);
            if (pending != txn->pending_writes().end()) {
                if (pending->second.deleted || pending->second.current == nullptr) {
                    continue;
                }
                if (encoded_key(index, *pending->second.current) != encoded) {
                    continue;
                }
            }
            throw UniqueConstraintError(table);
        }

        // Other active transactions reserve their prospective keys even though
        // those keys are not visible in the shared B+ tree yet.
        for (const auto &[other_id, other] : txn_map) {
            if (other_id == txn->get_transaction_id() || other == nullptr ||
                other->get_state() == TransactionState::ABORTED ||
                other->get_state() == TransactionState::COMMITTED) {
                continue;
            }
            for (const auto &[row_key, pending] : other->pending_writes()) {
                if (row_key.table != table || pending.deleted ||
                    pending.current == nullptr) {
                    continue;
                }
                const std::string encoded = encoded_key(index, *pending.current);
                if (desired_keys.find(encoded) != desired_keys.end()) {
                    throw TransactionAbortException(
                        txn->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
                }
            }
        }
    }

    // Reusing a key deleted after this transaction's snapshot is a stale-write
    // conflict, while a currently present committed duplicate was already
    // reported as a normal uniqueness violation above.
    for (const auto &key : candidate_keys) {
        const auto latest = latest_unique_commit_states_.find(key);
        if (latest != latest_unique_commit_states_.end() &&
            !latest->second.present &&
            latest->second.commit_ts > txn->get_start_ts()) {
            throw TransactionAbortException(txn->get_transaction_id(),
                                            AbortReason::WRITE_WRITE_CONFLICT);
        }
    }

    // Validation and reservation form one critical section.
    for (const auto &key : candidate_keys) {
        unique_write_owners_[key] = txn->get_transaction_id();
    }
}

bool TransactionManager::record_matches(const RmRecord &record,
                                        const std::vector<Condition> &conditions,
                                        const std::vector<PredicateColumn> &columns) const {
    for (const auto &condition : conditions) {
        auto lhs = std::find_if(columns.begin(), columns.end(), [&](const PredicateColumn &col) {
            return col.name == condition.lhs_col.col_name &&
                   (condition.lhs_col.tab_name.empty() || col.tab_name == condition.lhs_col.tab_name);
        });
        if (lhs == columns.end()) return false;
        const char *rhs_data = nullptr;
        ColType rhs_type{};
        int rhs_len = 0;
        if (condition.is_rhs_val) {
            rhs_data = condition.rhs_val.raw->data;
            rhs_type = condition.rhs_val.type;
            rhs_len = lhs->len;
        } else {
            auto rhs = std::find_if(columns.begin(), columns.end(), [&](const PredicateColumn &col) {
                return col.name == condition.rhs_col.col_name &&
                       (condition.rhs_col.tab_name.empty() || col.tab_name == condition.rhs_col.tab_name);
            });
            if (rhs == columns.end()) return false;
            rhs_data = record.data + rhs->offset;
            rhs_type = rhs->type;
            rhs_len = rhs->len;
        }
        const int cmp = compare_bytes(record.data + lhs->offset, lhs->type, lhs->len,
                                      rhs_data, rhs_type, rhs_len);
        if (!compare_result(cmp, condition.op)) return false;
    }
    return true;
}

bool TransactionManager::write_event_matches_predicate(
    const MvccWriteEvent &event, const PredicateRead &predicate) const {
    if (event.key.table != predicate.table) return false;
    if (event.has_before && event.before != nullptr &&
        record_matches(*event.before, predicate.conditions, predicate.columns)) return true;
    if (event.has_after && event.after != nullptr &&
        record_matches(*event.after, predicate.conditions, predicate.columns)) return true;
    return false;
}

bool TransactionManager::transactions_overlap(const Transaction *a,
                                              const Transaction *b) const {
    const timestamp_t a_end = a->get_commit_ts() == INVALID_TS
                                  ? std::numeric_limits<timestamp_t>::max()
                                  : a->get_commit_ts();
    const timestamp_t b_end = b->get_commit_ts() == INVALID_TS
                                  ? std::numeric_limits<timestamp_t>::max()
                                  : b->get_commit_ts();
    return a->get_start_ts() <= b_end && b->get_start_ts() <= a_end;
}

void TransactionManager::add_rw_dependency(Transaction *reader,
                                           Transaction *writer,
                                           Transaction *current_statement_txn) {
    if (reader == nullptr || writer == nullptr || reader == writer) return;
    if (reader->get_state() == TransactionState::ABORTED ||
        writer->get_state() == TransactionState::ABORTED) return;
    if (!transactions_overlap(reader, writer)) return;
    if (reader->rw_out().count(writer->get_transaction_id()) != 0) return;
    reader->add_rw_out(writer->get_transaction_id());
    writer->add_rw_in(reader->get_transaction_id());
    if (has_dangerous_structure()) {
        throw TransactionAbortException(current_statement_txn->get_transaction_id(),
                                        AbortReason::SERIALIZATION_FAILURE);
    }
}

bool TransactionManager::has_dangerous_structure() const {
    for (const auto &[pivot_id, pivot] : txn_map) {
        (void)pivot_id;
        if (pivot == nullptr || pivot->get_state() == TransactionState::ABORTED) continue;
        for (txn_id_t in_id : pivot->rw_in()) {
            auto in_it = txn_map.find(in_id);
            if (in_it == txn_map.end() || in_it->second == nullptr ||
                in_it->second->get_state() == TransactionState::ABORTED) continue;
            Transaction *tin = in_it->second;
            for (txn_id_t out_id : pivot->rw_out()) {
                auto out_it = txn_map.find(out_id);
                if (out_it == txn_map.end() || out_it->second == nullptr ||
                    out_it->second->get_state() == TransactionState::ABORTED) continue;
                Transaction *tout = out_it->second;
                if (!transactions_overlap(tin, pivot) ||
                    !transactions_overlap(pivot, tout)) continue;
                if (tin == tout) return true;
                const timestamp_t out_commit = tout->get_commit_ts();
                const timestamp_t in_commit = tin->get_commit_ts();
                if (out_commit != INVALID_TS &&
                    (in_commit == INVALID_TS || out_commit < in_commit)) return true;
            }
        }
    }
    return false;
}

void TransactionManager::compare_new_write_with_readers(Transaction *writer,
                                                        const MvccWriteEvent &event) {
    for (const auto &[id, reader] : txn_map) {
        (void)id;
        if (reader == nullptr || reader == writer ||
            reader->get_isolation_level() != IsolationLevel::SERIALIZABLE ||
            reader->get_state() == TransactionState::ABORTED) continue;
        if (!transactions_overlap(reader, writer)) continue;
        bool conflict = reader->record_reads().count(event.key) != 0;
        if (!conflict) {
            for (const auto &predicate : reader->predicate_reads()) {
                if (write_event_matches_predicate(event, predicate)) {
                    conflict = true;
                    break;
                }
            }
        }
        if (conflict) add_rw_dependency(reader, writer, writer);
    }
}

void TransactionManager::compare_new_read_with_writers(
    Transaction *reader, const PredicateRead *predicate,
    const TxnRowKey *record_key) {
    for (const auto &[id, writer] : txn_map) {
        (void)id;
        if (writer == nullptr || writer == reader ||
            writer->get_isolation_level() != IsolationLevel::SERIALIZABLE ||
            writer->get_state() == TransactionState::ABORTED) continue;
        const bool invisible = writer->get_commit_ts() == INVALID_TS ||
                               writer->get_commit_ts() > reader->get_start_ts();
        if (!invisible || !transactions_overlap(reader, writer)) continue;
        bool conflict = false;
        for (const auto &event : writer->write_events()) {
            if (record_key != nullptr && event.key == *record_key) conflict = true;
            if (predicate != nullptr && write_event_matches_predicate(event, *predicate)) conflict = true;
            if (conflict) break;
        }
        if (conflict) add_rw_dependency(reader, writer, reader);
    }
}

void TransactionManager::register_predicate_read(
    Transaction *txn, const std::string &table,
    const std::vector<Condition> &conditions,
    const std::vector<ColMeta> &columns) {
    if (txn == nullptr || !txn->get_txn_mode() ||
        txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) return;
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    PredicateRead read;
    read.table = table;
    read.conditions = conditions;
    read.columns.reserve(columns.size());
    for (const auto &column : columns) {
        read.columns.push_back({column.tab_name, column.name, column.type,
                                column.len, column.offset});
    }
    txn->add_predicate_read(read);
    compare_new_read_with_writers(txn, &read, nullptr);
}

void TransactionManager::register_record_read(Transaction *txn,
                                              const std::string &table,
                                              const Rid &rid) {
    if (txn == nullptr || !txn->get_txn_mode() ||
        txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) return;
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    const TxnRowKey key{table, rid};
    txn->add_record_read(key);
    compare_new_read_with_writers(txn, nullptr, &key);
}

void TransactionManager::release_write_owners(Transaction *txn) {
    for (auto it = write_owners_.begin(); it != write_owners_.end();) {
        if (it->second == txn->get_transaction_id()) it = write_owners_.erase(it);
        else ++it;
    }
    for (auto it = unique_write_owners_.begin();
         it != unique_write_owners_.end();) {
        if (it->second == txn->get_transaction_id()) {
            it = unique_write_owners_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = deleted_value_owners_.begin();
         it != deleted_value_owners_.end();) {
        if (it->second == txn->get_transaction_id()) {
            it = deleted_value_owners_.erase(it);
        } else {
            ++it;
        }
    }
}

void TransactionManager::update_deleted_value_commit_states(
    Transaction *txn, timestamp_t commit_ts) {
    std::unordered_set<TxnDeletedValueKey, TxnDeletedValueKeyHash> reinserted;
    for (const auto &row_key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(row_key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;
        if (!sm_manager_->db_.get_table(row_key.table).indexes.empty()) continue;
        if (!pending.deleted && pending.current != nullptr) {
            reinserted.insert(deleted_value_key(row_key.table, *pending.current));
        }
    }

    for (const auto &row_key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(row_key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;
        if (pending.inserted || !pending.deleted || pending.before == nullptr ||
            !sm_manager_->db_.get_table(row_key.table).indexes.empty()) {
            continue;
        }
        const TxnDeletedValueKey key =
            deleted_value_key(row_key.table, *pending.before);
        // DELETE followed by INSERT of the same value in one transaction is a
        // replacement, not a committed absence visible to stale writers.
        if (reinserted.count(key) == 0) {
            latest_deleted_value_commit_ts_[key] = commit_ts;
        }
    }
}

bool TransactionManager::committed_unique_key_exists(
    const TxnUniqueKey &key) const {
    const TabMeta &tab = sm_manager_->db_.get_table(key.table);
    for (const auto &index : tab.indexes) {
        if (index_identity(index) != key.index_id) continue;
        const std::string name = sm_manager_->get_ix_manager()->get_index_name(
            key.table, index.cols);
        auto handle = sm_manager_->ihs_.find(name);
        if (handle == sm_manager_->ihs_.end()) return false;
        std::vector<Rid> result;
        return handle->second->get_value(key.encoded_key.data(), &result, nullptr) &&
               !result.empty();
    }
    return false;
}

void TransactionManager::update_unique_commit_states(
    Transaction *txn, timestamp_t commit_ts) {
    std::unordered_set<TxnUniqueKey, TxnUniqueKeyHash> touched;
    for (const auto &row_key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(row_key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;

        // INSERT followed by DELETE in the same transaction has no committed
        // effect and must not make the key look newer to old snapshots.
        if (pending.inserted && pending.deleted) continue;

        if (!pending.inserted && pending.before != nullptr) {
            for (const auto &key :
                 unique_keys_for_record(row_key.table, *pending.before)) {
                touched.insert(key);
            }
        }
        if (!pending.deleted && pending.current != nullptr) {
            for (const auto &key :
                 unique_keys_for_record(row_key.table, *pending.current)) {
                touched.insert(key);
            }
        }
    }

    // apply_commit() has already installed the final table/index image.  Read
    // that image once per touched key so DELETE+INSERT replacements, key-moving
    // UPDATEs, and multi-row statements all record the correct final state.
    for (const auto &key : touched) {
        latest_unique_commit_states_[key] =
            UniqueKeyCommitState{commit_ts, committed_unique_key_exists(key)};
    }
}

void TransactionManager::apply_commit(Transaction *txn, timestamp_t commit_ts,
                                      LogManager *log_manager) {
    Context commit_context(lock_manager_, log_manager, txn, nullptr, nullptr, this);
    struct InsertedRow {
        std::string table;
        Rid rid;
        std::shared_ptr<RmRecord> record;
    };
    std::vector<InsertedRow> inserted;

    // Remove old index entries first so a transaction can move several unique
    // keys without transient conflicts during commit.
    for (const auto &key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;
        if (pending.inserted || pending.before == nullptr) continue;
        const TabMeta &tab = sm_manager_->db_.get_table(key.table);
        for (const auto &index : tab.indexes) {
            const std::string name = sm_manager_->get_ix_manager()->get_index_name(
                key.table, index.cols);
            auto old_key = make_index_key(index, pending.before->data);
            sm_manager_->ihs_.at(name)->delete_entry(old_key.data(), txn);
        }
    }

    // Table modifications receive a real Context so RmFileHandle emits the
    // physical before/after-image WAL records before dirtying each page.
    for (const auto &key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;
        RmFileHandle *fh = sm_manager_->fhs_.at(key.table).get();
        if (pending.inserted) {
            if (!pending.deleted && pending.current != nullptr) {
                const Rid actual = fh->insert_record(pending.current->data, &commit_context);
                inserted.push_back({key.table, actual, pending.current});
                latest_commit_ts_[{key.table, actual}] = commit_ts;
            }
        } else if (pending.deleted) {
            if (fh->is_record(key.rid)) fh->delete_record(key.rid, &commit_context);
            latest_commit_ts_[key] = commit_ts;
        } else if (pending.current != nullptr) {
            fh->update_record(key.rid, pending.current->data, &commit_context);
            latest_commit_ts_[key] = commit_ts;
        }
    }

    for (const auto &key : txn->pending_order()) {
        const auto it = txn->pending_writes().find(key);
        if (it == txn->pending_writes().end()) continue;
        const PendingWrite &pending = it->second;
        if (pending.inserted || pending.deleted || pending.current == nullptr) continue;
        const TabMeta &tab = sm_manager_->db_.get_table(key.table);
        for (const auto &index : tab.indexes) {
            const std::string name = sm_manager_->get_ix_manager()->get_index_name(
                key.table, index.cols);
            auto new_key = make_index_key(index, pending.current->data);
            if (sm_manager_->ihs_.at(name)->insert_entry(new_key.data(), key.rid, txn) ==
                IX_NO_PAGE) {
                throw UniqueConstraintError(key.table);
            }
        }
    }
    for (const auto &row : inserted) {
        const TabMeta &tab = sm_manager_->db_.get_table(row.table);
        for (const auto &index : tab.indexes) {
            const std::string name = sm_manager_->get_ix_manager()->get_index_name(
                row.table, index.cols);
            auto key = make_index_key(index, row.record->data);
            if (sm_manager_->ihs_.at(name)->insert_entry(key.data(), row.rid, txn) ==
                IX_NO_PAGE) {
                throw UniqueConstraintError(row.table);
            }
        }
    }
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) return;
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    const bool has_writes = !txn->pending_writes().empty();
    if (has_writes && log_manager != nullptr) {
        BeginLogRecord begin_log(txn->get_transaction_id(), txn->get_prev_lsn());
        const lsn_t lsn = log_manager->add_log_to_buffer(&begin_log);
        txn->set_prev_lsn(lsn);
    }

    const timestamp_t commit_ts = next_timestamp_++;
    apply_commit(txn, commit_ts, log_manager);
    update_unique_commit_states(txn, commit_ts);
    update_deleted_value_commit_states(txn, commit_ts);
    last_commit_ts_ = commit_ts;
    txn->set_commit_ts(commit_ts);
    release_write_owners(txn);
    txn->clear_write_set();
    release_transaction_locks(txn, lock_manager_);

    if (has_writes && log_manager != nullptr) {
        CommitLogRecord commit_log(txn->get_transaction_id(), txn->get_prev_lsn());
        const lsn_t lsn = log_manager->add_log_to_buffer(&commit_log);
        txn->set_prev_lsn(lsn);
        // The COMMIT record must reach the WAL file before success is returned.
        // Static checkpoints and graceful shutdown perform the stronger fsync.
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::COMMITTED);
    txn->set_txn_mode(false);
    txn->pending_writes().clear();
    active_txn_ids_.erase(txn->get_transaction_id());
    prune_finished_transactions();
}

void TransactionManager::cleanup_aborted_dependencies(Transaction *txn) {
    for (txn_id_t to : txn->rw_out()) {
        auto it = txn_map.find(to);
        if (it != txn_map.end() && it->second != nullptr) {
            it->second->remove_rw_in(txn->get_transaction_id());
        }
    }
    for (txn_id_t from : txn->rw_in()) {
        auto it = txn_map.find(from);
        if (it != txn_map.end() && it->second != nullptr) {
            it->second->remove_rw_out(txn->get_transaction_id());
        }
    }
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) return;
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    release_write_owners(txn);
    cleanup_aborted_dependencies(txn);
    txn->clear_write_set();
    txn->clear_mvcc_runtime();
    release_transaction_locks(txn, lock_manager_);
    if (log_manager != nullptr) log_manager->flush_log_to_disk();
    txn->set_state(TransactionState::ABORTED);
    txn->set_txn_mode(false);
    active_txn_ids_.erase(txn->get_transaction_id());
    prune_finished_transactions();
}

bool TransactionManager::is_committed_deleted(const std::string &table,
                                               const Rid &rid) const {
    (void)table;
    (void)rid;
    // The current main branch uses physical DELETE, not retained MVCC
    // tombstones. Keep this compatibility hook for recovery/checkpoint code.
    return false;
}

std::vector<CheckpointTombstone> TransactionManager::snapshot_tombstones() const {
    return {};
}

void TransactionManager::restore_tombstones(
    const std::vector<CheckpointTombstone> &tombstones) {
    (void)tombstones;
}

void TransactionManager::set_recovered_tombstone(const std::string &table,
                                                  const Rid &rid,
                                                  bool deleted) {
    (void)table;
    (void)rid;
    (void)deleted;
}

std::vector<txn_id_t> TransactionManager::active_transaction_ids(
    txn_id_t exclude) const {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    std::vector<txn_id_t> result;
    result.reserve(active_txn_ids_.size());
    for (txn_id_t txn_id : active_txn_ids_) {
        if (txn_id != exclude) result.push_back(txn_id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void TransactionManager::abort_all_active(LogManager *log_manager,
                                          txn_id_t exclude) {
    std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
    std::vector<Transaction *> active;
    active.reserve(active_txn_ids_.size());
    for (txn_id_t txn_id : active_txn_ids_) {
        if (txn_id == exclude) continue;
        auto it = txn_map.find(txn_id);
        if (it != txn_map.end() && it->second != nullptr) active.push_back(it->second);
    }
    for (Transaction *txn : active) abort(txn, log_manager);
}

void TransactionManager::reclaim_retired_transactions() {
    std::vector<Transaction *> retired;
    {
        std::lock_guard<std::recursive_mutex> guard(mvcc_latch_);
        retired.swap(retired_txns_);
    }
    for (Transaction *txn : retired) delete txn;
}

void TransactionManager::set_next_txn_id(txn_id_t next_txn_id) {
    txn_id_t current = next_txn_id_.load();
    while (current < next_txn_id &&
           !next_txn_id_.compare_exchange_weak(current, next_txn_id)) {
    }
}

// Compatibility implementations for framework MVCC helper APIs.
bool TransactionManager::UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) {
    (void)rid; (void)prev_link; (void)check; return true;
}
bool TransactionManager::UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    (void)rid; (void)prev_version; (void)check; return true;
}
std::optional<UndoLink> TransactionManager::GetUndoLink(Rid rid) { (void)rid; return std::nullopt; }
std::optional<VersionUndoLink> TransactionManager::GetVersionLink(Rid rid) { (void)rid; return std::nullopt; }
std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    auto it = txn_map.find(link.prev_txn_);
    if (it == txn_map.end() || it->second == nullptr) return std::nullopt;
    return it->second->GetUndoLog(link.prev_log_idx_);
}
UndoLog TransactionManager::GetUndoLog(UndoLink link) { return *GetUndoLogOptional(link); }
timestamp_t TransactionManager::GetWatermark() { return last_commit_ts_.load(); }
void TransactionManager::GarbageCollection() {}
