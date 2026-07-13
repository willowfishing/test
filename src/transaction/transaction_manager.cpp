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
#include "execution/execution_common.h"
#include "common/index_runtime.h"
#include "common/snapshot_index_history.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};
std::shared_mutex TransactionManager::txn_map_mutex_;

namespace {
std::mutex version_latch;
constexpr size_t kTxnGcScheduleThreshold = 64;
constexpr size_t kTxnGcBatchLimit = 512;
constexpr size_t kRetiredTupleGcScheduleThreshold = 1024;
constexpr size_t kRetiredTupleGcBatchLimit = 4096;
constexpr size_t kTableVersionCacheSize = 8;

struct TableVersionCacheEntry {
    TransactionManager *manager{nullptr};
    uint64_t manager_id{0};
    uint64_t epoch{0};
    std::string table_name;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info;
};

thread_local std::array<TableVersionCacheEntry, kTableVersionCacheSize> table_version_cache;

TableVersionCacheEntry &table_version_cache_entry(const std::string &table_name) {
    return table_version_cache[std::hash<std::string>{}(table_name) & (kTableVersionCacheSize - 1)];
}

template <typename Fn>
class ScopeExit {
   public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ~ScopeExit() { fn_(); }

   private:
    Fn fn_;
};

std::string index_key(const IndexMeta &index, const char *record_data) {
    std::string key;
    for (int i = 0; i < index.col_num; ++i) {
        const auto &col = index.cols[i];
        key += col.name;
        key.push_back('\x1f');
        key.append(record_data + col.offset, col.len);
        key.push_back('\x1e');
    }
    return key;
}

std::string raw_index_key(const IndexMeta &index, const char *record_data) {
    std::string key(index.logical_col_tot_len(), '\0');
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(&key[offset], record_data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

const ColMeta *logical_identity_col(const TabMeta &tab) {
    auto id_col = std::find_if(tab.cols.begin(), tab.cols.end(), [](const ColMeta &col) {
        return col.name == "id";
    });
    if (id_col != tab.cols.end()) {
        return &*id_col;
    }
    if (tab.cols.size() == 1) {
        return &tab.cols.front();
    }
    return nullptr;
}

std::string logical_key(const TabMeta &tab, const RmRecord &record) {
    const ColMeta *col = logical_identity_col(tab);
    if (col == nullptr) {
        return {};
    }
    std::string key = "\x1dlogical\x1f";
    key += col->name;
    key.push_back('\x1f');
    key.append(record.data + col->offset, col->len);
    return key;
}

void delete_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings, const RmRecord &record,
                                const Rid &rid, Transaction *txn) {
    std::vector<std::string> key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        key_scratch[i].resize(binding.meta->col_tot_len);
        char *key = rmdb::build_index_key_into(*binding.meta, record.data, rid, &key_scratch[i]);
        binding.ih->delete_entry(key, txn);
    }
}

void delete_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                          const RmRecord &record, const Rid &rid, Transaction *txn) {
    auto bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    delete_index_entries_bound(bindings, record, rid, txn);
}


void release_locks(LockManager *lock_manager, Transaction *txn) {
    if (lock_manager == nullptr || txn == nullptr) {
        return;
    }
    lock_manager->unlock_all(txn);
}

bool same_rid(const Rid &lhs, const Rid &rhs) {
    return lhs.page_no == rhs.page_no && lhs.slot_no == rhs.slot_no;
}

bool has_read_record(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    const auto &read_records = txn->get_read_records();
    auto iter = read_records.find(tab_name);
    if (iter == read_records.end()) {
        return false;
    }
    for (const auto &read_rid : iter->second) {
        if (same_rid(read_rid, rid)) {
            return true;
        }
    }
    return false;
}

bool has_written_record(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr) {
        return false;
    }
    for (const auto &entry : txn->get_serializable_writes()) {
        const auto &write_info = entry.second;
        if (write_info.tab_name == tab_name && same_rid(write_info.rid, rid)) {
            return true;
        }
    }
    return false;
}

bool is_overlapping_serializable(Transaction *other, Transaction *txn) {
    if (other == nullptr || other == txn || other->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return false;
    }
    if (other->get_state() == TransactionState::GROWING) {
        return true;
    }
    return other->get_state() == TransactionState::COMMITTED &&
           other->get_commit_ts() != INVALID_TS &&
           other->get_commit_ts() > txn->get_start_ts();
}

bool writer_invisible_to_reader(Transaction *reader, Transaction *writer) {
    if (reader == nullptr || writer == nullptr || reader == writer) {
        return false;
    }
    if (!is_overlapping_serializable(writer, reader)) {
        return false;
    }
    if (writer->get_state() == TransactionState::GROWING) {
        return true;
    }
    return writer->get_state() == TransactionState::COMMITTED &&
           writer->get_commit_ts() != INVALID_TS &&
           writer->get_commit_ts() > reader->get_read_ts();
}

bool record_matches(const std::vector<ColMeta> &cols, const RmRecord &record, const std::vector<Condition> &conds) {
    auto find_col = [&](const TabCol &target) {
        return std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
    };
    auto compare = [](const char *lhs, const char *rhs, ColType type, int len) {
        if (type == TYPE_INT) {
            int a = *reinterpret_cast<const int *>(lhs);
            int b = *reinterpret_cast<const int *>(rhs);
            return (a > b) - (a < b);
        }
        if (type == TYPE_FLOAT) {
            float a = *reinterpret_cast<const float *>(lhs);
            float b = *reinterpret_cast<const float *>(rhs);
            return (a > b) - (a < b);
        }
        return memcmp(lhs, rhs, len);
    };
    auto pass = [](int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    };
    for (const auto &cond : conds) {
        auto lhs_col = find_col(cond.lhs_col);
        if (lhs_col == cols.end()) {
            return false;
        }
        const char *rhs = cond.is_rhs_val ? cond.rhs_val.raw->data : nullptr;
        if (!cond.is_rhs_val) {
            auto rhs_col = find_col(cond.rhs_col);
            if (rhs_col == cols.end()) {
                return false;
            }
            rhs = record.data + rhs_col->offset;
        }
        if (!pass(compare(record.data + lhs_col->offset, rhs, lhs_col->type, lhs_col->len), cond.op)) {
            return false;
        }
    }
    return true;
}

bool commits_before(Transaction *tout, Transaction *tin) {
    return tout != nullptr && tout->get_state() == TransactionState::COMMITTED &&
           tout->get_commit_ts() != INVALID_TS &&
           (tin == nullptr || tin->get_state() != TransactionState::COMMITTED ||
            tout->get_commit_ts() < tin->get_commit_ts());
}

bool has_dangerous_structure_after_new_edge(Transaction *from, Transaction *to) {
    if (from == nullptr || to == nullptr) {
        return false;
    }

    for (auto next_id : to->get_rw_dependencies()) {
        auto next_iter = TransactionManager::txn_map.find(next_id);
        if (next_iter == TransactionManager::txn_map.end()) {
            continue;
        }
        auto *tout = next_iter->second;
        if (tout == nullptr || tout->get_state() == TransactionState::ABORTED) {
            continue;
        }
        if (tout == from || commits_before(tout, from)) {
            return true;
        }
    }

    for (const auto &entry : TransactionManager::txn_map) {
        auto *tin = entry.second;
        if (tin == nullptr || tin == from || tin->get_state() == TransactionState::ABORTED) {
            continue;
        }
        if (tin->get_rw_dependencies().count(from->get_transaction_id()) == 0) {
            continue;
        }
        if (to == tin || commits_before(to, tin)) {
            return true;
        }
    }
    return false;
}

void add_dependency(Transaction *from, Transaction *to, Transaction *abort_candidate) {
    if (from == nullptr || to == nullptr || from == to) {
        return;
    }
    if (from->get_rw_dependencies().count(to->get_transaction_id()) != 0) {
        return;
    }
    from->add_rw_dependency(to->get_transaction_id());
    if (has_dangerous_structure_after_new_edge(from, to)) {
        throw TransactionAbortException(abort_candidate->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
}

bool use_mvcc_snapshot_read(Transaction *txn) {
    return txn != nullptr && (txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
                              txn->get_isolation_level() == IsolationLevel::SERIALIZABLE);
}

bool use_si_conflict_checks(Transaction *txn) {
    return txn != nullptr && (txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
                              txn->get_isolation_level() == IsolationLevel::SERIALIZABLE);
}

bool uses_watermark(IsolationLevel isolation_level) {
    (void)isolation_level;
    // READ COMMITTED readers can transiently need an undo image when they race an uncommitted writer.
    // Tracking every active transaction keeps that image alive until the reader finishes its statement.
    return true;
}

bool tuple_version_visible_to_txn(const TupleMeta &meta, Transaction *txn) {
    if (txn == nullptr || !use_mvcc_snapshot_read(txn)) {
        return true;
    }
    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        return owner == txn->get_transaction_id();
    }
    return meta.ts_ <= txn->get_read_ts();
}

bool tuple_version_visible_read_committed(const TupleMeta &meta, Transaction *txn, timestamp_t visible_commit_ts) {
    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        return txn != nullptr && owner == txn->get_transaction_id();
    }
    return meta.ts_ <= visible_commit_ts;
}

bool keys_overlap(const std::vector<std::string> &lhs, const std::vector<std::string> &rhs) {
    for (const auto &key : lhs) {
        if (std::find(rhs.begin(), rhs.end(), key) != rhs.end()) {
            return true;
        }
    }
    return false;
}

bool is_logical_identity_key(const std::string &key) {
    static const std::string prefix = "\x1dlogical\x1f";
    return key.compare(0, prefix.size(), prefix) == 0;
}

bool nonlogical_keys_overlap(const std::vector<std::string> &lhs, const std::vector<std::string> &rhs) {
    for (const auto &key : lhs) {
        if (is_logical_identity_key(key)) {
            continue;
        }
        if (std::find(rhs.begin(), rhs.end(), key) != rhs.end()) {
            return true;
        }
    }
    return false;
}

inline void bump_page_visibility_epoch(const std::shared_ptr<TransactionManager::PageVersionInfo> &page_info) {
    if (page_info != nullptr) {
        page_info->visibility_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
}

inline void mark_dirty_slot(const std::shared_ptr<TransactionManager::TableVersionInfo> &table_info,
                            const std::shared_ptr<TransactionManager::PageVersionInfo> &page_info,
                            const Rid &rid) {
    if (page_info == nullptr || rid.slot_no < 0) {
        return;
    }
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (!page_info->CanTrackDirtySlot(slot)) {
        if (table_info != nullptr) {
            table_info->mark_page_dirty_exact(rid.page_no);
        }
        return;
    }
    bool newly_dirty = page_info->MarkDirtySlot(slot);
    if (newly_dirty && table_info != nullptr) {
        table_info->dirty_slot_total_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (newly_dirty && page_info->DirtySlotCount() == 1 && table_info != nullptr) {
        table_info->mark_page_dirty_exact(rid.page_no);
    }
}

inline void clear_dirty_slot_if_empty(const std::shared_ptr<TransactionManager::TableVersionInfo> &table_info,
                                      const std::shared_ptr<TransactionManager::PageVersionInfo> &page_info,
                                      const Rid &rid) {
    if (page_info == nullptr || rid.slot_no < 0) {
        return;
    }
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (!page_info->CanTrackDirtySlot(slot)) {
        return;
    }
    if (!page_info->HasTupleMeta(slot) && !page_info->HasVersion(slot)) {
        bool cleared = page_info->ClearDirtySlot(slot);
        if (cleared && table_info != nullptr) {
            table_info->dirty_slot_total_.fetch_sub(1, std::memory_order_acq_rel);
        }
        if (cleared && page_info->DirtySlotCount() == 0 && table_info != nullptr) {
            table_info->clear_page_dirty_exact(rid.page_no);
        }
    }
}

void init_page_dirty_slots(SmManager *sm_manager, const std::string &tab_name,
                           const std::shared_ptr<TransactionManager::PageVersionInfo> &page_info) {
    if (sm_manager == nullptr || page_info == nullptr) {
        return;
    }
    auto fh_iter = sm_manager->fhs_.find(tab_name);
    if (fh_iter == sm_manager->fhs_.end() || fh_iter->second == nullptr) {
        return;
    }
    int slot_count = fh_iter->second->get_file_hdr().num_records_per_page;
    if (slot_count <= 0) {
        return;
    }
    page_info->InitDirtySlots(static_cast<uint32_t>(slot_count));
}
}

TransactionDrainGuard::~TransactionDrainGuard() {
    reset();
}

TransactionDrainGuard::TransactionDrainGuard(TransactionDrainGuard &&other) noexcept : manager_(other.manager_) {
    other.manager_ = nullptr;
}

TransactionDrainGuard &TransactionDrainGuard::operator=(TransactionDrainGuard &&other) noexcept {
    if (this != &other) {
        reset();
        manager_ = other.manager_;
        other.manager_ = nullptr;
    }
    return *this;
}

void TransactionDrainGuard::reset() {
    if (manager_ != nullptr) {
        manager_->ReleaseTransactionDrain();
        manager_ = nullptr;
    }
}

void delete_changed_old_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                                      const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                      Transaction *txn);
void delete_changed_old_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn);

void delete_changed_new_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                                      const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                      Transaction *txn);
void delete_changed_new_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn);

bool TransactionManager::UpdateTupleMeta(const std::string &tab_name, Rid rid, std::optional<TupleMeta> meta,
                                         std::function<bool(std::optional<TupleMeta>)> &&check) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Tuple metadata slot is out of range");
    }
    std::optional<TupleMeta> current_meta = std::nullopt;
    if (const TupleMeta *stored_meta = page_info->GetTupleMeta(slot); stored_meta != nullptr) {
        current_meta = *stored_meta;
    }
    if (check != nullptr && !check(current_meta)) {
        return false;
    }
    const bool was_deleted = current_meta.has_value() && current_meta->is_deleted_;
    const bool will_be_deleted = meta.has_value() && meta->is_deleted_;
    const bool was_uncommitted = current_meta.has_value() && current_meta->ts_ >= TXN_START_ID;
    const bool will_be_uncommitted = meta.has_value() && meta->ts_ >= TXN_START_ID;
    bool changed = current_meta.has_value() != meta.has_value();
    if (!changed && current_meta.has_value() && meta.has_value()) {
        changed = current_meta->ts_ != meta->ts_ || current_meta->is_deleted_ != meta->is_deleted_;
    }
    if (changed) {
        bump_page_visibility_epoch(page_info);
    }
    if (!current_meta.has_value() && meta.has_value()) {
        page_info->active_meta_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (current_meta.has_value() && !meta.has_value()) {
        page_info->active_meta_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (!was_uncommitted && will_be_uncommitted) {
        page_info->uncommitted_meta_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (was_uncommitted && !will_be_uncommitted) {
        page_info->uncommitted_meta_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (meta.has_value() && meta->ts_ < TXN_START_ID) {
        timestamp_t observed = page_info->max_committed_meta_ts_.load(std::memory_order_relaxed);
        while (meta->ts_ > observed &&
               !page_info->max_committed_meta_ts_.compare_exchange_weak(
                   observed, meta->ts_, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
    if (!was_deleted && will_be_deleted) {
        page_info->deleted_count_.fetch_add(1, std::memory_order_relaxed);
    }
    if (meta.has_value()) {
        mark_dirty_slot(table_info, page_info, rid);
        page_info->SetTupleMeta(slot, *meta);
    } else {
        page_info->ClearTupleMeta(slot);
        clear_dirty_slot_if_empty(table_info, page_info, rid);
        page_info->ReleaseTupleMetaStorageIfEmpty();
    }
    if (was_deleted && !will_be_deleted) {
        page_info->deleted_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (changed) {
        bump_page_visibility_epoch(page_info);
    }
    return true;
}

std::optional<TupleMeta> TransactionManager::GetTupleMeta(const std::string &tab_name, Rid rid) {
    auto table_info = GetTableVersionInfo(tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, rid.page_no);
    if (page_info == nullptr) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    if (rid.slot_no < 0) {
        return std::nullopt;
    }
    const TupleMeta *meta = page_info->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
    return meta == nullptr ? std::nullopt : std::optional<TupleMeta>(*meta);
}

TupleMeta TransactionManager::GetTupleMetaOrDefault(const std::string &tab_name, Rid rid) {
    auto meta = GetTupleMeta(tab_name, rid);
    if (meta.has_value()) {
        return *meta;
    }
    return TupleMeta{0, false};
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfo(
    const std::string &tab_name, page_id_t page_no) {
    return GetPageVersionInfoOnTable(GetTableVersionInfo(tab_name), page_no);
}

std::shared_ptr<TransactionManager::TableVersionInfo> TransactionManager::GetTableVersionInfo(
    const std::string &tab_name) {
    uint64_t epoch = version_info_epoch_.load(std::memory_order_acquire);
    auto &cached = table_version_cache_entry(tab_name);
    if (cached.manager == this && cached.manager_id == version_cache_id_ && cached.epoch == epoch &&
        cached.table_name == tab_name) {
        return cached.table_info;
    }

    std::shared_lock<std::shared_mutex> version_lock(version_info_mutex_);
    auto table_iter = version_info_.find(tab_name);
    std::shared_ptr<TableVersionInfo> table_info =
        table_iter == version_info_.end() ? nullptr : table_iter->second;
    cached = TableVersionCacheEntry{this, version_cache_id_, epoch, tab_name, table_info};
    return table_info;
}

std::shared_ptr<TransactionManager::TableVersionInfo> TransactionManager::GetOrCreateTableVersionInfo(
    const std::string &tab_name) {
    {
        std::shared_lock<std::shared_mutex> version_lock(version_info_mutex_);
        auto table_iter = version_info_.find(tab_name);
        if (table_iter != version_info_.end() && table_iter->second != nullptr) {
            return table_iter->second;
        }
    }
    std::unique_lock<std::shared_mutex> version_lock(version_info_mutex_);
    auto &table_info = version_info_[tab_name];
    if (table_info == nullptr) {
        table_info = std::make_shared<TableVersionInfo>();
        version_info_epoch_.fetch_add(1, std::memory_order_release);
    }
    return table_info;
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no) {
    return GetPageVersionInfoOnTable(table_info, page_no, nullptr);
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetPageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, uint64_t *page_map_epoch) {
    if (table_info == nullptr) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = 0;
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no) && !table_info->is_page_dirty_exact(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no)) {
        auto *dense_page_info = table_info->lookup_page_info_dense(page_no);
        if (dense_page_info != nullptr) {
            if (page_map_epoch != nullptr) {
                *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
            }
            std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
            auto page_iter = table_info->pages_.find(page_no);
            if (page_iter != table_info->pages_.end()) {
                return page_iter->second;
            }
        }
    }
    if (!table_info->maybe_has_page_version_info(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
    if (page_map_epoch != nullptr) {
        *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
    }
    auto page_iter = table_info->pages_.find(page_no);
    if (page_iter == table_info->pages_.end()) {
        return nullptr;
    }
    return page_iter->second;
}

TransactionManager::PageVersionInfo *TransactionManager::GetPageVersionInfoOnTableRaw(
    const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, uint64_t *page_map_epoch) {
    if (table_info == nullptr) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = 0;
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no) && !table_info->is_page_dirty_exact(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    if (table_info->tracks_exact_dirty_page(page_no)) {
        auto *dense_page_info = table_info->lookup_page_info_dense(page_no);
        if (dense_page_info != nullptr) {
            if (page_map_epoch != nullptr) {
                *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
            }
            return dense_page_info;
        }
    }
    if (!table_info->maybe_has_page_version_info(page_no)) {
        if (page_map_epoch != nullptr) {
            *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
    if (page_map_epoch != nullptr) {
        *page_map_epoch = table_info->page_map_epoch_.load(std::memory_order_acquire);
    }
    auto page_iter = table_info->pages_.find(page_no);
    if (page_iter == table_info->pages_.end()) {
        return nullptr;
    }
    return page_iter->second.get();
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetOrCreatePageVersionInfo(
    const std::string &tab_name, page_id_t page_no) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    return GetOrCreatePageVersionInfoOnTable(table_info, tab_name, page_no);
}

std::shared_ptr<TransactionManager::PageVersionInfo> TransactionManager::GetOrCreatePageVersionInfoOnTable(
    const std::shared_ptr<TableVersionInfo> &table_info, const std::string &tab_name, page_id_t page_no) {
    table_info->mark_page_version_info(page_no);
    {
        std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
        auto page_iter = table_info->pages_.find(page_no);
        if (page_iter != table_info->pages_.end() && page_iter->second != nullptr) {
            return page_iter->second;
        }
    }

    std::unique_lock<std::shared_mutex> table_lock(table_info->mutex_);
    auto &page_info = table_info->pages_[page_no];
    if (page_info == nullptr) {
        auto new_page_info = std::make_shared<PageVersionInfo>();
        init_page_dirty_slots(sm_manager_, tab_name, new_page_info);
        page_info = std::move(new_page_info);
        table_info->publish_page_info_dense(page_no, page_info.get());
        table_info->page_map_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    return page_info;
}

void TransactionManager::RetainUndoReference(const UndoLink &link) {
    if (!link.IsValid()) {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    auto txn_iter = txn_map.find(link.prev_txn_);
    if (txn_iter == txn_map.end() || txn_iter->second == nullptr) {
        throw InternalError("Cannot retain missing undo transaction");
    }
    txn_iter->second->RetainUndoReference();
}

void TransactionManager::ReleaseUndoReference(const UndoLink &link) {
    if (!link.IsValid()) {
        return;
    }
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    auto txn_iter = txn_map.find(link.prev_txn_);
    if (txn_iter == txn_map.end() || txn_iter->second == nullptr) {
        assert(false && "Cannot release missing undo transaction");
        return;
    }
    txn_iter->second->ReleaseUndoReference();
}

bool TransactionManager::UpdateUndoLink(const std::string &tab_name, Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    std::optional<UndoLink> current_link = std::nullopt;
    if (stored_version != nullptr && stored_version->prev_.IsValid()) {
        current_link = stored_version->prev_;
    }
    if (check != nullptr && !check(current_link)) {
        return false;
    }

    const UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    const UndoLink new_link = prev_link.has_value() ? *prev_link : UndoLink{};
    const bool link_changed = old_link != new_link;
    if (link_changed && new_link.IsValid()) {
        RetainUndoReference(new_link);
    }

    bool changed = false;
    bool mutation_succeeded = false;
    try {
        if (prev_link.has_value()) {
            changed = stored_version == nullptr || stored_version->prev_ != *prev_link;
            if (changed) {
                bump_page_visibility_epoch(page_info);
            }
            mark_dirty_slot(table_info, page_info, rid);
            if (stored_version == nullptr) {
                page_info->SetVersion(slot, VersionUndoLink{*prev_link, false});
                page_info->version_link_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                stored_version->prev_ = *prev_link;
            }
            if (changed) {
                bump_page_visibility_epoch(page_info);
            }
            mutation_succeeded = true;
            if (link_changed && old_link.IsValid()) {
                ReleaseUndoReference(old_link);
            }
            return true;
        }

        if (stored_version == nullptr) {
            mutation_succeeded = true;
            return true;
        }
        if (stored_version->in_progress_) {
            changed = stored_version->prev_.IsValid();
            if (changed) {
                bump_page_visibility_epoch(page_info);
            }
            stored_version->prev_ = UndoLink{};
        } else {
            changed = true;
            bump_page_visibility_epoch(page_info);
            page_info->ClearVersion(slot);
            page_info->version_link_count_.fetch_sub(1, std::memory_order_relaxed);
            clear_dirty_slot_if_empty(table_info, page_info, rid);
            page_info->ReleaseVersionStorageIfEmpty();
        }
        if (changed) {
            bump_page_visibility_epoch(page_info);
        }
        mutation_succeeded = true;
        if (link_changed && old_link.IsValid()) {
            ReleaseUndoReference(old_link);
        }
        return true;
    } catch (...) {
        if (!mutation_succeeded && link_changed && new_link.IsValid()) {
            ReleaseUndoReference(new_link);
        }
        throw;
    }
}

bool TransactionManager::UpdateVersionLink(const std::string &tab_name, Rid rid,
                                           std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    std::shared_ptr<PageVersionInfo> page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);

    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }
    std::optional<VersionUndoLink> current_version = std::nullopt;
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    if (stored_version != nullptr) {
        current_version = *stored_version;
    }
    if (check != nullptr && !check(current_version)) {
        return false;
    }

    const UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    const UndoLink new_link = prev_version.has_value() ? prev_version->prev_ : UndoLink{};
    const bool link_changed = old_link != new_link;
    if (link_changed && new_link.IsValid()) {
        RetainUndoReference(new_link);
    }

    bool changed = false;
    bool mutation_succeeded = false;
    try {
        if (prev_version.has_value()) {
            changed = stored_version == nullptr || *stored_version != *prev_version;
            if (changed) {
                bump_page_visibility_epoch(page_info);
            }
            mark_dirty_slot(table_info, page_info, rid);
            if (stored_version == nullptr) {
                page_info->version_link_count_.fetch_add(1, std::memory_order_relaxed);
            }
            page_info->SetVersion(slot, *prev_version);
        } else {
            if (stored_version != nullptr) {
                changed = true;
                bump_page_visibility_epoch(page_info);
                page_info->ClearVersion(slot);
                page_info->version_link_count_.fetch_sub(1, std::memory_order_relaxed);
                clear_dirty_slot_if_empty(table_info, page_info, rid);
                page_info->ReleaseVersionStorageIfEmpty();
            }
        }
        if (changed) {
            bump_page_visibility_epoch(page_info);
        }
        mutation_succeeded = true;
        if (link_changed && old_link.IsValid()) {
            ReleaseUndoReference(old_link);
        }
        return true;
    } catch (...) {
        if (!mutation_succeeded && link_changed && new_link.IsValid()) {
            ReleaseUndoReference(new_link);
        }
        throw;
    }
}

void TransactionManager::InstallTupleVersion(const std::string &tab_name, Rid rid,
                                              const VersionUndoLink &version, const TupleMeta &meta) {
    auto table_info = GetOrCreateTableVersionInfo(tab_name);
    auto page_info = GetOrCreatePageVersionInfoOnTable(table_info, tab_name, rid.page_no);
    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    slot_offset_t slot = static_cast<slot_offset_t>(rid.slot_no);
    if (rid.slot_no < 0 || !page_info->CanTrackSlot(slot)) {
        throw InternalError("Version metadata slot is out of range");
    }

    // Allocate both sparse stores before retaining or publishing anything so
    // allocation failure cannot leave a half-installed tuple version.
    page_info->EnsureVersionStorage();
    page_info->EnsureTupleMetaStorage();
    VersionUndoLink *stored_version = page_info->GetMutableVersion(slot);
    const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
    UndoLink old_link = stored_version == nullptr ? UndoLink{} : stored_version->prev_;
    bool link_changed = old_link != version.prev_;
    if (link_changed && version.prev_.IsValid()) {
        RetainUndoReference(version.prev_);
    }

    bump_page_visibility_epoch(page_info);
    mark_dirty_slot(table_info, page_info, rid);
    if (stored_version == nullptr) {
        page_info->version_link_count_.fetch_add(1, std::memory_order_relaxed);
    }
    page_info->SetVersion(slot, version);

    bool had_meta = stored_meta != nullptr;
    bool was_uncommitted = had_meta && stored_meta->ts_ >= TXN_START_ID;
    bool was_deleted = had_meta && stored_meta->is_deleted_;
    bool will_be_uncommitted = meta.ts_ >= TXN_START_ID;
    if (!had_meta) {
        page_info->active_meta_count_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!was_uncommitted && will_be_uncommitted) {
        page_info->uncommitted_meta_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (was_uncommitted && !will_be_uncommitted) {
        page_info->uncommitted_meta_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (!was_deleted && meta.is_deleted_) {
        page_info->deleted_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (was_deleted && !meta.is_deleted_) {
        page_info->deleted_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (meta.ts_ < TXN_START_ID) {
        timestamp_t observed = page_info->max_committed_meta_ts_.load(std::memory_order_relaxed);
        while (meta.ts_ > observed &&
               !page_info->max_committed_meta_ts_.compare_exchange_weak(
                   observed, meta.ts_, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
    page_info->SetTupleMeta(slot, meta);
    bump_page_visibility_epoch(page_info);
    if (link_changed && old_link.IsValid()) {
        ReleaseUndoReference(old_link);
    }
}

std::optional<UndoLink> TransactionManager::GetUndoLink(const std::string &tab_name, Rid rid) {
    auto version_link = GetVersionLink(tab_name, rid);
    if (!version_link.has_value() || !version_link->prev_.IsValid()) {
        return std::nullopt;
    }
    return version_link->prev_;
}

std::optional<VersionUndoLink> TransactionManager::GetVersionLink(const std::string &tab_name, Rid rid) {
    auto table_info = GetTableVersionInfo(tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, rid.page_no);
    if (page_info == nullptr) {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
    if (rid.slot_no < 0) {
        return std::nullopt;
    }
    const VersionUndoLink *version = page_info->GetVersion(static_cast<slot_offset_t>(rid.slot_no));
    return version == nullptr ? std::nullopt : std::optional<VersionUndoLink>(*version);
}

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    if (!link.IsValid()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    auto txn_iter = txn_map.find(link.prev_txn_);
    if (txn_iter == txn_map.end() || txn_iter->second == nullptr) {
        return std::nullopt;
    }
    Transaction *txn = txn_iter->second;
    if (link.prev_log_idx_ < 0 || static_cast<size_t>(link.prev_log_idx_) >= txn->GetUndoLogNum()) {
        return std::nullopt;
    }
    return txn->GetUndoLog(link.prev_log_idx_);
}

UndoLink TransactionManager::AppendUndoLog(Transaction *txn, UndoLog log) {
    if (txn == nullptr) {
        throw InternalError("Cannot append undo log without a transaction");
    }
    const UndoLink predecessor = log.prev_version_;
    const bool cross_transaction = predecessor.IsValid() &&
                                   predecessor.prev_txn_ != txn->get_transaction_id();
    if (cross_transaction) {
        RetainUndoReference(predecessor);
    }
    try {
        return txn->AppendUndoLog(std::move(log));
    } catch (...) {
        if (cross_transaction) {
            ReleaseUndoReference(predecessor);
        }
        throw;
    }
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto undo_log = GetUndoLogOptional(link);
    if (!undo_log.has_value()) {
        throw InternalError("Undo log not found");
    }
    return *undo_log;
}

timestamp_t TransactionManager::GetWatermark() {
    return running_txns_.GetWatermark();
}

void TransactionManager::EnqueueRetiredTuples(timestamp_t commit_ts, std::vector<RetiredTuple> tuples) {
    if (tuples.empty()) {
        return;
    }
    RetiredTransaction retired{commit_ts, std::move(tuples)};
    const size_t tuple_count = retired.tuples.size();
    std::lock_guard<std::mutex> gc_lock(gc_mutex_);
    auto insert_pos = std::upper_bound(
        retired_txn_gc_queue_.begin(), retired_txn_gc_queue_.end(), commit_ts,
        [](timestamp_t ts, const RetiredTransaction &entry) { return ts < entry.commit_ts; });
    retired_txn_gc_queue_.insert(insert_pos, std::move(retired));
    retired_tuple_count_.fetch_add(tuple_count, std::memory_order_release);
}

bool TransactionManager::ReclaimRetiredTuple(timestamp_t commit_ts, const RetiredTuple &retired) {
    auto table_info = GetTableVersionInfo(retired.tab_name);
    auto page_info = GetPageVersionInfoOnTable(table_info, retired.rid.page_no);
    if (table_info == nullptr || page_info == nullptr) {
        return true;
    }

    UndoLink released_link{};
    {
        std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
        if (retired.rid.slot_no < 0) {
            return true;
        }
        slot_offset_t slot = static_cast<slot_offset_t>(retired.rid.slot_no);
        const TupleMeta *stored_meta = page_info->GetTupleMeta(slot);
        if (stored_meta == nullptr || stored_meta->ts_ != commit_ts || stored_meta->ts_ >= TXN_START_ID) {
            return true;
        }

        const VersionUndoLink *stored_version = page_info->GetVersion(slot);
        if (stored_version != nullptr && stored_version->in_progress_) {
            return false;
        }

        const bool is_deleted = stored_meta->is_deleted_;
        if (is_deleted && sm_manager_ != nullptr) {
            auto fh_iter = sm_manager_->fhs_.find(retired.tab_name);
            if (fh_iter != sm_manager_->fhs_.end() && fh_iter->second != nullptr) {
                try {
                    if (fh_iter->second->is_record(retired.rid)) {
                        // The logical DELETE and its commit record are durable before the tuple is retired.
                        fh_iter->second->delete_record(retired.rid, nullptr);
                    }
                } catch (...) {
                    return false;
                }
            }
        }

        bump_page_visibility_epoch(page_info);
        page_info->ClearTupleMeta(slot);
        page_info->active_meta_count_.fetch_sub(1, std::memory_order_relaxed);
        if (is_deleted) {
            page_info->deleted_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        if (stored_version != nullptr) {
            released_link = stored_version->prev_;
            page_info->ClearVersion(slot);
            page_info->version_link_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        clear_dirty_slot_if_empty(table_info, page_info, retired.rid);
        page_info->ReleaseTupleMetaStorageIfEmpty();
        page_info->ReleaseVersionStorageIfEmpty();
        bump_page_visibility_epoch(page_info);
    }

    if (released_link.IsValid()) {
        ReleaseUndoReference(released_link);
    }
    return true;
}

void TransactionManager::QueueFinishedTransactionForGc(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    bool should_collect = false;
    {
        std::unique_lock<std::shared_mutex> lock(txn_map_mutex_);
        if (!txn->gc_ready() ||
            (txn->get_state() != TransactionState::COMMITTED &&
             txn->get_state() != TransactionState::ABORTED)) {
            return;
        }
        finished_txn_gc_queue_.push_back(txn->get_transaction_id());
        ++finished_txn_gc_since_last_;
        should_collect = use_mvcc_snapshot_read(txn) ||
                         retired_tuple_count_.load(std::memory_order_acquire) >=
                             kRetiredTupleGcScheduleThreshold ||
                         finished_txn_gc_queue_.size() >= kTxnGcScheduleThreshold ||
                         finished_txn_gc_since_last_ >= kTxnGcScheduleThreshold;
    }
    if (should_collect) {
        GarbageCollection();
    }
}

void TransactionManager::GarbageCollectFinishedTransactions() {
    std::vector<Transaction *> releasable;
    {
        std::unique_lock<std::shared_mutex> lock(txn_map_mutex_);
        if (finished_txn_gc_queue_.empty()) {
            return;
        }

        bool has_active_serializable = false;
        timestamp_t oldest_active_serializable_start_ts = INVALID_TS;
        for (const auto &entry : txn_map) {
            auto *active = entry.second;
            if (active == nullptr || active->get_state() != TransactionState::GROWING ||
                active->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
                continue;
            }
            has_active_serializable = true;
            if (oldest_active_serializable_start_ts == INVALID_TS ||
                active->get_start_ts() < oldest_active_serializable_start_ts) {
                oldest_active_serializable_start_ts = active->get_start_ts();
            }
        }

        size_t attempts = std::min(finished_txn_gc_queue_.size(), kTxnGcBatchLimit);
        finished_txn_gc_since_last_ = 0;
        for (size_t i = 0; i < attempts; ++i) {
            txn_id_t txn_id = finished_txn_gc_queue_.front();
            finished_txn_gc_queue_.pop_front();

            auto iter = txn_map.find(txn_id);
            if (iter == txn_map.end()) {
                continue;
            }
            auto *txn = iter->second;
            if (txn == nullptr || !txn->gc_ready() ||
                (txn->get_state() != TransactionState::COMMITTED &&
                 txn->get_state() != TransactionState::ABORTED)) {
                continue;
            }

            if (txn->GetUndoReferenceCount() == 0 && txn->GetUndoLogNum() > 0) {
                auto undo_logs = txn->TakeUndoLogs();
                for (const auto &undo_log : undo_logs) {
                    const UndoLink &predecessor = undo_log.prev_version_;
                    if (!predecessor.IsValid() || predecessor.prev_txn_ == txn_id) {
                        continue;
                    }
                    auto predecessor_iter = txn_map.find(predecessor.prev_txn_);
                    assert(predecessor_iter != txn_map.end() && predecessor_iter->second != nullptr);
                    if (predecessor_iter != txn_map.end() && predecessor_iter->second != nullptr) {
                        predecessor_iter->second->ReleaseUndoReference();
                    }
                }
            }

            if (!txn->ssi_metadata_released()) {
                bool aborted = txn->get_state() == TransactionState::ABORTED;
                bool snapshot_overlap_done = !has_active_serializable ||
                    (txn->get_commit_ts() != INVALID_TS &&
                     txn->get_commit_ts() <= oldest_active_serializable_start_ts);
                if (aborted || snapshot_overlap_done) {
                    for (auto &entry : txn_map) {
                        if (entry.second != nullptr && entry.second != txn) {
                            entry.second->remove_rw_dependency(txn_id);
                        }
                    }
                    txn->clear_serializable_state();
                    txn->set_ssi_metadata_released(true);
                }
            }

            bool undo_released = txn->GetUndoReferenceCount() == 0 && txn->GetUndoLogNum() == 0;
            if (undo_released && txn->ssi_metadata_released()) {
                releasable.push_back(txn);
                txn_map.erase(iter);
            } else {
                finished_txn_gc_queue_.push_back(txn_id);
            }
        }
    }

    for (auto *txn : releasable) {
        delete txn;
    }
}

std::optional<RmRecord> TransactionManager::GetVisibleTuple(const std::string &tab_name, const Rid &rid,
                                                            Transaction *txn, TupleMeta *visible_meta) {
    RmRecord visible_record;
    if (!GetVisibleTupleInto(tab_name, rid, txn, &visible_record, visible_meta)) {
        return std::nullopt;
    }
    return visible_record;
}

bool TransactionManager::GetVisibleTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                             RmRecord *out_record, TupleMeta *visible_meta) {
    RmRecord base_record;
    if (!sm_manager_->fhs_.at(tab_name)->read_record(rid, &base_record, nullptr)) {
        return false;
    }

    TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
    if (tuple_version_visible_to_txn(base_meta, txn)) {
        if (visible_meta != nullptr) {
            *visible_meta = base_meta;
        }
        if (base_meta.is_deleted_) {
            return false;
        }
        *out_record = base_record;
        return true;
    }

    auto version_link = GetVersionLink(tab_name, rid);
    while (version_link.has_value() && version_link->prev_.IsValid()) {
        auto undo_log = GetUndoLogOptional(version_link->prev_);
        if (!undo_log.has_value()) {
            break;
        }
        TupleMeta undo_meta{undo_log->ts_, undo_log->is_deleted_};
        if (tuple_version_visible_to_txn(undo_meta, txn)) {
            if (visible_meta != nullptr) {
                *visible_meta = undo_meta;
            }
            if (undo_meta.is_deleted_) {
                return false;
            }
            if (undo_log->tuple_test_ != nullptr) {
                *out_record = *undo_log->tuple_test_;
                return true;
            }
            auto reconstructed = ReconstructTuple(&sm_manager_->db_.get_table(tab_name), base_record, base_meta,
                                                  std::vector<UndoLog>{*undo_log});
            if (!reconstructed.has_value()) {
                return false;
            }
            *out_record = std::move(*reconstructed);
            return true;
        }
        if (!undo_log->prev_version_.IsValid()) {
            break;
        }
        version_link = VersionUndoLink::FromOptionalUndoLink(undo_log->prev_version_);
    }
    return false;
}

bool TransactionManager::GetReadCommittedTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                                   RmRecord *out_record, TupleMeta *visible_meta,
                                                   RmRecordPageCursor *page_cursor,
                                                   const ReadCommittedTupleHint *hint) {
    RmRecord base_record;
    bool found = false;
    if (page_cursor != nullptr) {
        found = page_cursor->read_record(rid, &base_record);
    } else {
        found = sm_manager_->fhs_.at(tab_name)->read_record(rid, &base_record, nullptr);
    }
    if (!found) {
        return false;
    }

    TupleMeta base_meta{0, false};
    std::optional<VersionUndoLink> version_link;
    bool used_hint = false;
    if (hint != nullptr && hint->Matches(rid)) {
        std::shared_lock<std::shared_mutex> page_lock(hint->page_info->mutex_);
        uint64_t current_epoch = hint->page_info->visibility_epoch_.load(std::memory_order_acquire);
        if (current_epoch == hint->visibility_epoch) {
            base_meta = hint->has_meta ? hint->meta : TupleMeta{0, false};
            if (hint->has_version_link) {
                version_link = hint->version_link;
            }
            used_hint = true;
        }
    }
    if (!used_hint) {
        base_meta = GetTupleMetaOrDefault(tab_name, rid);
    }
    timestamp_t visible_commit_ts = last_commit_ts_.load();
    if (tuple_version_visible_read_committed(base_meta, txn, visible_commit_ts)) {
        if (visible_meta != nullptr) {
            *visible_meta = base_meta;
        }
        if (base_meta.is_deleted_) {
            return false;
        }
        *out_record = base_record;
        return true;
    }

    if (!used_hint) {
        version_link = GetVersionLink(tab_name, rid);
    }
    while (version_link.has_value() && version_link->prev_.IsValid()) {
        auto undo_log = GetUndoLogOptional(version_link->prev_);
        if (!undo_log.has_value()) {
            break;
        }
        TupleMeta undo_meta{undo_log->ts_, undo_log->is_deleted_};
        if (tuple_version_visible_read_committed(undo_meta, txn, visible_commit_ts)) {
            if (visible_meta != nullptr) {
                *visible_meta = undo_meta;
            }
            if (undo_meta.is_deleted_) {
                return false;
            }
            if (undo_log->tuple_test_ != nullptr) {
                *out_record = *undo_log->tuple_test_;
                return true;
            }
            auto reconstructed = ReconstructTuple(&sm_manager_->db_.get_table(tab_name), base_record, base_meta,
                                                  std::vector<UndoLog>{*undo_log});
            if (!reconstructed.has_value()) {
                return false;
            }
            *out_record = std::move(*reconstructed);
            return true;
        }
        if (!undo_log->prev_version_.IsValid()) {
            break;
        }
        version_link = VersionUndoLink::FromOptionalUndoLink(undo_log->prev_version_);
    }
    return false;
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifyReadCommittedIndexEntry(
    const std::string &tab_name, const Rid &rid, Transaction *txn) {
    auto page_info = GetPageVersionInfo(tab_name, rid.page_no);
    return ClassifyReadCommittedIndexEntryOnPage(page_info, rid, txn);
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifyReadCommittedIndexEntryOnPage(
    const std::shared_ptr<PageVersionInfo> &page_info, const Rid &rid, Transaction *txn) {
    return ClassifyReadCommittedIndexEntryOnPage(page_info.get(), rid, txn);
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifyReadCommittedIndexEntryOnPage(
    PageVersionInfo *page_info, const Rid &rid, Transaction *txn, ReadCommittedTupleHint *hint) {
    if (hint != nullptr) {
        hint->Reset();
    }
    if (page_info == nullptr) {
        return ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }
    if (IsReadCommittedPageCleanVisible(page_info)) {
        return ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }

    TupleMeta meta{INVALID_TS, false};
    bool has_meta = false;
    bool has_version_link = false;
    {
        std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
        const TupleMeta *stored_meta = rid.slot_no < 0
                                           ? nullptr
                                           : page_info->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
        if (stored_meta != nullptr) {
            meta = *stored_meta;
            has_meta = true;
        }
        const VersionUndoLink *stored_version = rid.slot_no < 0
                                                    ? nullptr
                                                    : page_info->GetVersion(static_cast<slot_offset_t>(rid.slot_no));
        has_version_link = stored_version != nullptr;
        if (hint != nullptr) {
            hint->valid = true;
            hint->page_no = rid.page_no;
            hint->slot_no = rid.slot_no;
            hint->page_info = page_info;
            hint->visibility_epoch = page_info->visibility_epoch_.load(std::memory_order_acquire);
            hint->has_meta = has_meta;
            hint->meta = has_meta ? meta : TupleMeta{0, false};
            hint->has_version_link = has_version_link;
            if (has_version_link) {
                hint->version_link = *stored_version;
            }
        }
    }

    return ClassifyReadCommittedIndexEntryState(has_meta, meta, has_version_link, txn);
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifyReadCommittedIndexEntryState(
    std::optional<TupleMeta> meta, bool has_version_link, Transaction *txn) const {
    if (!meta.has_value()) {
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }

    return ClassifyReadCommittedIndexEntryState(true, *meta, has_version_link, txn);
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifyReadCommittedIndexEntryState(
    bool has_meta, const TupleMeta &meta, bool has_version_link, Transaction *txn) const {
    if (!has_meta) {
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }

    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        bool owned_by_self = txn != nullptr && owner == txn->get_transaction_id();
        if (owned_by_self) {
            if (meta.is_deleted_) {
                return ReadCommittedIndexEntryState::INVISIBLE;
            }
            return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                    : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::INVISIBLE;
    }

    timestamp_t visible_commit_ts = last_commit_ts_.load();
    if (meta.ts_ > visible_commit_ts) {
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::INVISIBLE;
    }
    if (meta.is_deleted_) {
        return ReadCommittedIndexEntryState::INVISIBLE;
    }
    return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                            : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifySnapshotIndexEntryOnPage(
    PageVersionInfo *page_info, const Rid &rid, Transaction *txn, ReadCommittedTupleHint *hint) {
    if (hint != nullptr) {
        hint->Reset();
    }
    if (page_info == nullptr || IsSnapshotPageCleanVisible(page_info, txn)) {
        return ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }

    TupleMeta meta{INVALID_TS, false};
    bool has_meta = false;
    bool has_version_link = false;
    {
        std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
        const TupleMeta *stored_meta = rid.slot_no < 0
                                           ? nullptr
                                           : page_info->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
        if (stored_meta != nullptr) {
            meta = *stored_meta;
            has_meta = true;
        }
        const VersionUndoLink *stored_version = rid.slot_no < 0
                                                    ? nullptr
                                                    : page_info->GetVersion(static_cast<slot_offset_t>(rid.slot_no));
        has_version_link = stored_version != nullptr;
        if (hint != nullptr) {
            hint->valid = true;
            hint->page_no = rid.page_no;
            hint->slot_no = rid.slot_no;
            hint->page_info = page_info;
            hint->visibility_epoch = page_info->visibility_epoch_.load(std::memory_order_acquire);
            hint->has_meta = has_meta;
            hint->meta = has_meta ? meta : TupleMeta{0, false};
            hint->has_version_link = has_version_link;
            if (has_version_link) {
                hint->version_link = *stored_version;
            }
        }
    }
    return ClassifySnapshotIndexEntryState(has_meta, meta, has_version_link, txn);
}

TransactionManager::ReadCommittedIndexEntryState TransactionManager::ClassifySnapshotIndexEntryState(
    bool has_meta, const TupleMeta &meta, bool has_version_link, Transaction *txn) const {
    if (!has_meta) {
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
    }
    if (meta.ts_ >= TXN_START_ID) {
        txn_id_t owner = meta.ts_ - TXN_START_ID;
        if (txn != nullptr && owner == txn->get_transaction_id()) {
            if (meta.is_deleted_) {
                return ReadCommittedIndexEntryState::INVISIBLE;
            }
            return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                    : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::INVISIBLE;
    }
    timestamp_t read_ts = txn == nullptr ? last_commit_ts_.load() : txn->get_read_ts();
    if (meta.ts_ > read_ts) {
        return has_version_link ? ReadCommittedIndexEntryState::NEEDS_HEAP
                                : ReadCommittedIndexEntryState::INVISIBLE;
    }
    return meta.is_deleted_ ? ReadCommittedIndexEntryState::INVISIBLE
                            : ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
}

bool TransactionManager::IsReadCommittedPageCleanVisible(
    const std::shared_ptr<PageVersionInfo> &page_info) const {
    return IsReadCommittedPageCleanVisible(page_info.get());
}

bool TransactionManager::IsReadCommittedPageCleanVisible(
    const PageVersionInfo *page_info) const {
    if (page_info == nullptr) {
        return true;
    }
    timestamp_t visible_commit_ts = last_commit_ts_.load();
    return page_info->max_committed_meta_ts_.load(std::memory_order_relaxed) <= visible_commit_ts &&
           page_info->uncommitted_meta_count_.load(std::memory_order_relaxed) <= 0 &&
           page_info->deleted_count_.load(std::memory_order_relaxed) <= 0 &&
           page_info->version_link_count_.load(std::memory_order_relaxed) <= 0;
}

bool TransactionManager::IsSnapshotPageCleanVisible(const PageVersionInfo *page_info, Transaction *txn) const {
    if (page_info == nullptr) {
        return true;
    }
    timestamp_t read_ts = txn == nullptr ? last_commit_ts_.load() : txn->get_read_ts();
    return page_info->max_committed_meta_ts_.load(std::memory_order_relaxed) <= read_ts &&
           page_info->uncommitted_meta_count_.load(std::memory_order_relaxed) <= 0 &&
           page_info->deleted_count_.load(std::memory_order_relaxed) <= 0;
}

void TransactionManager::EnsureWriteConflictFree(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr) {
        return;
    }
    TupleMeta current_meta = GetTupleMetaOrDefault(tab_name, rid);
    EnsureWriteConflictFree(txn, current_meta);
}

void TransactionManager::EnsureWriteConflictFree(Transaction *txn, const TupleMeta &current_meta) {
    if (txn == nullptr) {
        return;
    }
    if (current_meta.ts_ >= TXN_START_ID &&
        current_meta.ts_ - TXN_START_ID != txn->get_transaction_id()) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    if (!use_si_conflict_checks(txn)) {
        return;
    }
    if (IsWriteWriteConflict(current_meta.ts_, txn)) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
}

void TransactionManager::EnsureKeyConflictFree(Transaction *txn, const std::string &tab_name, const TabMeta &tab,
                                               const RmRecord &record, const Rid *self_rid) {
    if (!use_si_conflict_checks(txn) || sm_manager_ == nullptr) {
        return;
    }
    auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
    for (const auto &binding : bindings) {
        const auto &index = *binding.meta;
        if (!index.unique) {
            continue;
        }
        std::string candidate_key = index_key(index, record.data);
        std::vector<Rid> matches;
        binding.ih->get_value(candidate_key.data(), &matches, txn);
        std::vector<std::string> col_names;
        col_names.reserve(index.cols.size());
        for (const auto &col : index.cols) {
            col_names.push_back(col.name);
        }
        auto historical = rmdb::lookup_snapshot_index_history(tab_name, col_names, txn->get_read_ts());
        matches.insert(matches.end(), historical.begin(), historical.end());
        auto conflict = ClassifyUniqueIndexConflict(txn, tab_name, index, candidate_key.data(), matches, self_rid);
        if (conflict == UniqueKeyConflictResult::ABORT) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    const ColMeta *identity_col = logical_identity_col(tab);
    if (identity_col == nullptr) {
        return;
    }
    bool identity_indexed = std::any_of(tab.indexes.begin(), tab.indexes.end(), [&](const IndexMeta &index) {
        return index.unique && index.col_num == 1 && index.cols.front().name == identity_col->name;
    });
    if (identity_indexed) {
        return;
    }

    const std::vector<std::string> candidate_keys{logical_key(tab, record)};
    auto &fh = sm_manager_->fhs_.at(tab_name);
    for (RmScan scan(fh.get()); !scan.is_end(); scan.next()) {
        Rid rid = scan.rid();
        if (self_rid != nullptr && rid == *self_rid) {
            continue;
        }
        auto base_record = fh->get_record(rid, nullptr);
        TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
        const std::vector<std::string> base_keys{logical_key(tab, *base_record)};
        auto visible_record = GetVisibleTuple(tab_name, rid, txn);
        bool uncommitted_other = base_meta.ts_ >= TXN_START_ID &&
                                 base_meta.ts_ - TXN_START_ID != txn->get_transaction_id();
        bool newer_committed = base_meta.ts_ < TXN_START_ID && base_meta.ts_ > txn->get_read_ts();
        bool base_visible = tuple_version_visible_to_txn(base_meta, txn);

        if (visible_record.has_value()) {
            const std::vector<std::string> visible_keys{logical_key(tab, *visible_record)};
            if (keys_overlap(candidate_keys, visible_keys)) {
                bool self_owned = base_meta.ts_ >= TXN_START_ID &&
                                  base_meta.ts_ - TXN_START_ID == txn->get_transaction_id();
                if (self_owned && !nonlogical_keys_overlap(candidate_keys, visible_keys)) {
                    continue;
                }
                if (!base_visible || uncommitted_other || newer_committed) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
                continue;
            }
            if (keys_overlap(candidate_keys, base_keys) && (uncommitted_other || newer_committed)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            continue;
        }

        if (!keys_overlap(candidate_keys, base_keys)) {
            continue;
        }

        if (uncommitted_other || newer_committed) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }
}

TransactionManager::UniqueKeyConflictResult TransactionManager::ClassifyUniqueIndexConflict(
    Transaction *txn, const std::string &tab_name, const IndexMeta &index, const char *key,
    const std::vector<Rid> &matches, const Rid *self_rid) {
    if (!index.unique) {
        return UniqueKeyConflictResult::NONE;
    }
    if (matches.empty()) {
        return UniqueKeyConflictResult::NONE;
    }
    std::string candidate_key(key, index.col_tot_len);
    if (txn == nullptr || sm_manager_ == nullptr) {
        for (const auto &rid : matches) {
            if (self_rid == nullptr || rid != *self_rid) {
                return UniqueKeyConflictResult::FAILURE;
            }
        }
        return UniqueKeyConflictResult::NONE;
    }

    auto &fh = sm_manager_->fhs_.at(tab_name);
    if (txn->get_isolation_level() != IsolationLevel::SNAPSHOT_ISOLATION &&
        txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        for (const auto &rid : matches) {
            if (self_rid != nullptr && rid == *self_rid) {
                continue;
            }
            std::unique_ptr<RmRecord> base_record;
            try {
                base_record = fh->get_record(rid, nullptr);
            } catch (...) {
                continue;
            }
            TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
            bool uncommitted = base_meta.ts_ >= TXN_START_ID;
            bool uncommitted_other = uncommitted && base_meta.ts_ - TXN_START_ID != txn->get_transaction_id();
            bool uncommitted_self = uncommitted && base_meta.ts_ - TXN_START_ID == txn->get_transaction_id();

            RmRecord visible_record;
            if (GetReadCommittedTupleInto(tab_name, rid, txn, &visible_record)) {
                std::string visible_key = raw_index_key(index, visible_record.data);
                if (visible_key == candidate_key) {
                    return uncommitted_other ? UniqueKeyConflictResult::ABORT
                                             : UniqueKeyConflictResult::FAILURE;
                }
            }

            std::string base_key = raw_index_key(index, base_record->data);
            if (base_key == candidate_key && uncommitted_other) {
                return UniqueKeyConflictResult::ABORT;
            }
            if (base_key == candidate_key && uncommitted_self) {
                return UniqueKeyConflictResult::FAILURE;
            }
        }
        return UniqueKeyConflictResult::NONE;
    }

    for (const auto &rid : matches) {
        if (self_rid != nullptr && rid == *self_rid) {
            continue;
        }
        auto base_record = fh->get_record(rid, nullptr);
        TupleMeta base_meta = GetTupleMetaOrDefault(tab_name, rid);
        auto visible_record = GetVisibleTuple(tab_name, rid, txn);

        bool uncommitted_other = base_meta.ts_ >= TXN_START_ID &&
                                 base_meta.ts_ - TXN_START_ID != txn->get_transaction_id();
        bool newer_committed = base_meta.ts_ < TXN_START_ID && base_meta.ts_ > txn->get_read_ts();
        std::string base_key = raw_index_key(index, base_record->data);
        bool base_visible = tuple_version_visible_to_txn(base_meta, txn);

        if (visible_record.has_value()) {
            std::string visible_key = raw_index_key(index, visible_record->data);
            if (visible_key == candidate_key) {
                if (base_visible) {
                    return UniqueKeyConflictResult::FAILURE;
                }
                return UniqueKeyConflictResult::ABORT;
            }
            if (base_key == candidate_key && (uncommitted_other || newer_committed)) {
                return UniqueKeyConflictResult::ABORT;
            }
            continue;
        }

        if (base_key == candidate_key && (uncommitted_other || newer_committed)) {
            return UniqueKeyConflictResult::ABORT;
        }
        return UniqueKeyConflictResult::NONE;
    }
    return UniqueKeyConflictResult::NONE;
}

void TransactionManager::GarbageCollection() {
    std::unique_lock<std::mutex> run_lock(gc_run_mutex_, std::try_to_lock);
    if (!run_lock.owns_lock()) {
        return;
    }

    const timestamp_t watermark = GetWatermark();
    rmdb::purge_snapshot_index_history(watermark);
    std::vector<RetiredTransaction> ready;
    size_t ready_tuple_count = 0;
    {
        std::lock_guard<std::mutex> gc_lock(gc_mutex_);
        while (!retired_txn_gc_queue_.empty() &&
               retired_txn_gc_queue_.front().commit_ts <= watermark) {
            size_t next_count = retired_txn_gc_queue_.front().tuples.size();
            if (!ready.empty() && ready_tuple_count + next_count > kRetiredTupleGcBatchLimit) {
                break;
            }
            ready_tuple_count += next_count;
            ready.push_back(std::move(retired_txn_gc_queue_.front()));
            retired_txn_gc_queue_.pop_front();
            retired_tuple_count_.fetch_sub(next_count, std::memory_order_release);
        }
    }

    std::vector<RetiredTransaction> retry;
    for (auto &retired_txn : ready) {
        RetiredTransaction failed{retired_txn.commit_ts, {}};
        for (const auto &retired : retired_txn.tuples) {
            if (!ReclaimRetiredTuple(retired_txn.commit_ts, retired)) {
                failed.tuples.push_back(retired);
            }
        }
        if (!failed.tuples.empty()) {
            retry.push_back(std::move(failed));
        }
    }
    for (auto &failed : retry) {
        EnqueueRetiredTuples(failed.commit_ts, std::move(failed.tuples));
    }

    GarbageCollectFinishedTransactions();
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager, IsolationLevel isolation_level) {
    {
        std::unique_lock<std::mutex> admission_lock(admission_mutex_);
        admission_cv_.wait(admission_lock, [&] { return !transaction_admission_blocked_; });
        ++active_transaction_count_;
    }

    try {
        if (txn == nullptr) {
            txn = new Transaction(next_txn_id_++, isolation_level);
        } else {
            txn->set_isolation_level(isolation_level);
        }
        txn->set_state(TransactionState::GROWING);
        txn->set_start_ts(next_timestamp_++);
    if (txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION || txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) { capture_snapshot(txn); }
        txn->set_read_ts(last_commit_ts_.load());
        txn->set_commit_ts(INVALID_TS);
        txn->set_watermark_registered(uses_watermark(isolation_level));
        txn->set_gc_ready(false);
        txn->set_admission_active(true);
        txn->set_ssi_metadata_released(isolation_level != IsolationLevel::SERIALIZABLE);
        if (txn->watermark_registered()) {
            running_txns_.AddTxn(txn->get_read_ts());
        }

        std::unique_lock<std::shared_mutex> lock(txn_map_mutex_);
        txn_map[txn->get_transaction_id()] = txn;
        if (log_manager != nullptr) {
            BeginLogRecord log_record(txn->get_transaction_id());
            txn->set_prev_lsn(log_manager->add_log_to_buffer(&log_record));
        }
        return txn;
    } catch (...) {
        if (txn != nullptr) {
            txn->set_admission_active(false);
        }
        CancelTransactionAdmissionReservation();
        throw;
    }
}

TransactionDrainGuard TransactionManager::BlockNewTransactionsAndWait() {
    std::unique_lock<std::mutex> admission_lock(admission_mutex_);
    admission_cv_.wait(admission_lock, [&] { return !transaction_admission_blocked_; });
    transaction_admission_blocked_ = true;
    admission_cv_.wait(admission_lock, [&] { return active_transaction_count_ == 0; });
    return TransactionDrainGuard(this);
}

TransactionDrainGuard TransactionManager::TryBlockNewTransactionsIfIdle() {
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (transaction_admission_blocked_ || active_transaction_count_ != 0) {
        return {};
    }
    transaction_admission_blocked_ = true;
    return TransactionDrainGuard(this);
}

TransactionDrainGuard TransactionManager::BlockNewTransactionsAndWaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> admission_lock(admission_mutex_);
    if (transaction_admission_blocked_) {
        return {};
    }
    transaction_admission_blocked_ = true;
    if (!admission_cv_.wait_for(admission_lock, timeout, [&] { return active_transaction_count_ == 0; })) {
        transaction_admission_blocked_ = false;
        admission_lock.unlock();
        admission_cv_.notify_all();
        return {};
    }
    return TransactionDrainGuard(this);
}

size_t TransactionManager::ActiveTransactionCount() const {
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    return active_transaction_count_;
}

void TransactionManager::FinishTransactionAdmission(Transaction *txn) {
    if (txn == nullptr || !txn->admission_active()) {
        return;
    }
    txn->set_admission_active(false);
    CancelTransactionAdmissionReservation();
}

void TransactionManager::CancelTransactionAdmissionReservation() {
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    assert(active_transaction_count_ > 0);
    --active_transaction_count_;
    admission_cv_.notify_all();
}

void TransactionManager::ReleaseTransactionDrain() {
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    transaction_admission_blocked_ = false;
    admission_cv_.notify_all();
}

std::vector<CheckpointTxnInfo> TransactionManager::CollectActiveTxnCheckpointInfo(Transaction *exclude_txn) {
    std::vector<CheckpointTxnInfo> active_txns;
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    active_txns.reserve(txn_map.size());
    for (const auto &entry : txn_map) {
        auto *txn = entry.second;
        if (txn == nullptr || txn == exclude_txn || txn->get_state() != TransactionState::GROWING) {
            continue;
        }
        active_txns.push_back(CheckpointTxnInfo{entry.first, txn->get_prev_lsn()});
    }
    return active_txns;
}

void TransactionManager::record_serializable_read(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
    txn->add_read_record(tab_name, rid);
    for (auto &entry : txn_map) {
        Transaction *other = entry.second;
        if (!writer_invisible_to_reader(txn, other)) {
            continue;
        }
        if (has_written_record(other, tab_name, rid)) {
            add_dependency(txn, other, txn);
        }
    }
}

void TransactionManager::record_serializable_predicate_read(Transaction *txn, const std::string &tab_name,
                                                           const std::vector<ColMeta> &cols,
                                                           const std::vector<Condition> &conds) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
    txn->add_predicate_read(tab_name, cols, conds);
    for (auto &entry : txn_map) {
        Transaction *other = entry.second;
        if (!writer_invisible_to_reader(txn, other)) {
            continue;
        }
        for (const auto &write_entry : other->get_serializable_writes()) {
            const auto &write_info = write_entry.second;
            if (write_info.tab_name != tab_name) {
                continue;
            }
            bool matches_old = write_info.old_record != nullptr &&
                               record_matches(cols, *write_info.old_record, conds);
            bool matches_new = write_info.new_record != nullptr &&
                               record_matches(cols, *write_info.new_record, conds);
            if (matches_old || matches_new) {
                add_dependency(txn, other, txn);
                break;
            }
        }
    }
}

void TransactionManager::record_serializable_write(Transaction *txn, const std::string &tab_name, const Rid &rid,
                                                  const RmRecord *old_record, const RmRecord *new_record,
                                                  const std::vector<ColMeta> *cols) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
    std::shared_ptr<RmRecord> old_copy = old_record == nullptr ? nullptr : std::make_shared<RmRecord>(*old_record);
    std::shared_ptr<RmRecord> new_copy = new_record == nullptr ? nullptr : std::make_shared<RmRecord>(*new_record);
    txn->upsert_serializable_write(tab_name, rid, cols == nullptr ? std::vector<ColMeta>() : *cols,
                                   std::move(old_copy), std::move(new_copy));

    for (auto &entry : txn_map) {
        Transaction *other = entry.second;
        if (!is_overlapping_serializable(other, txn)) {
            continue;
        }
        if (has_read_record(other, tab_name, rid)) {
            add_dependency(other, txn, txn);
        }
        if (cols == nullptr) {
            continue;
        }
        for (const auto &predicate : other->get_predicate_reads()) {
            bool matches_old = old_record != nullptr && predicate.tab_name == tab_name &&
                               record_matches(*cols, *old_record, predicate.conds);
            bool matches_new = new_record != nullptr && predicate.tab_name == tab_name &&
                               record_matches(*cols, *new_record, predicate.conds);
            if (matches_old || matches_new) {
                add_dependency(other, txn, txn);
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
    bool admission_finished = false;
    ScopeExit completion_guard([&] {
        if (!admission_finished &&
            (txn->get_state() == TransactionState::COMMITTED || txn->get_state() == TransactionState::ABORTED)) {
            FinishTransactionAdmission(txn);
        }
    });
    auto &write_set = txn->get_write_set();
    const bool has_writes = !write_set.empty();

    struct PendingCommitTuple {
        std::string tab_name;
        Rid rid;
        bool is_deleted;
        bool publish_version;
        size_t write_sequence;
    };
    struct PendingCommitSlot {
        slot_offset_t slot;
        bool is_deleted;
        bool publish_version;
    };
    struct CommitPageBatch {
        std::string tab_name;
        page_id_t page_no;
        std::shared_ptr<TableVersionInfo> table_info;
        std::shared_ptr<PageVersionInfo> page_info;
        std::vector<PendingCommitSlot> slots;
    };

    std::vector<PendingCommitTuple> pending_tuples;
    pending_tuples.reserve(write_set.size());
    size_t write_sequence = 0;
    for (auto &write_record : write_set) {
        const std::string &tab_name = write_record.GetTableName();
        const Rid &rid = write_record.GetRid();
        WType write_type = write_record.GetWriteType();
        pending_tuples.push_back(PendingCommitTuple{
            tab_name, rid, write_type == WType::DELETE_TUPLE, write_type != WType::INSERT_TUPLE, write_sequence++});
    }
    std::sort(pending_tuples.begin(), pending_tuples.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.tab_name != rhs.tab_name) {
            return lhs.tab_name < rhs.tab_name;
        }
        if (lhs.rid.page_no != rhs.rid.page_no) {
            return lhs.rid.page_no < rhs.rid.page_no;
        }
        if (lhs.rid.slot_no != rhs.rid.slot_no) {
            return lhs.rid.slot_no < rhs.rid.slot_no;
        }
        return lhs.write_sequence < rhs.write_sequence;
    });
    size_t compacted_count = 0;
    for (size_t i = 0; i < pending_tuples.size(); ++i) {
        PendingCommitTuple &pending = pending_tuples[i];
        if (compacted_count > 0 && pending_tuples[compacted_count - 1].tab_name == pending.tab_name &&
            pending_tuples[compacted_count - 1].rid == pending.rid) {
            PendingCommitTuple &compacted = pending_tuples[compacted_count - 1];
            compacted.is_deleted = pending.is_deleted;
            compacted.publish_version = compacted.publish_version || pending.publish_version;
            continue;
        }
        if (compacted_count != i) {
            pending_tuples[compacted_count] = std::move(pending);
        }
        ++compacted_count;
    }
    pending_tuples.resize(compacted_count);

    std::vector<CommitPageBatch> page_batches;
    for (const auto &pending : pending_tuples) {
        if (page_batches.empty() || page_batches.back().tab_name != pending.tab_name ||
            page_batches.back().page_no != pending.rid.page_no) {
            auto table_info = GetOrCreateTableVersionInfo(pending.tab_name);
            auto page_info = GetOrCreatePageVersionInfoOnTable(
                table_info, pending.tab_name, pending.rid.page_no);
            page_batches.push_back(CommitPageBatch{
                pending.tab_name, pending.rid.page_no, std::move(table_info), std::move(page_info), {}});
        }
        slot_offset_t slot = static_cast<slot_offset_t>(pending.rid.slot_no);
        if (pending.rid.slot_no < 0 || !page_batches.back().page_info->CanTrackSlot(slot)) {
            throw InternalError("Tuple metadata slot is out of range");
        }
        page_batches.back().slots.push_back(PendingCommitSlot{slot, pending.is_deleted, pending.publish_version});
    }

    if (log_manager != nullptr && has_writes) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t commit_lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(commit_lsn);
        // Publish tuple metadata only after the commit log is durable.
        log_manager->flush_log_to_disk_until_group(commit_lsn + log_record.log_tot_len_ - 1);
    }

    {
        std::lock_guard<std::mutex> lock(version_latch);
        txn->set_commit_ts(next_timestamp_++);

        timestamp_t commit_version = txn->get_commit_ts();
        for (auto &batch : page_batches) {
            std::unique_lock<std::shared_mutex> page_lock(batch.page_info->mutex_);
            bump_page_visibility_epoch(batch.page_info);
            for (const auto &[slot, is_deleted, publish_version] : batch.slots) {
                TupleMeta *current_meta = batch.page_info->GetMutableTupleMeta(slot);
                bool had_meta = current_meta != nullptr;
                bool was_uncommitted = had_meta && current_meta->ts_ >= TXN_START_ID;
                bool was_deleted = had_meta && current_meta->is_deleted_;
                if (!had_meta) {
                    Rid rid{batch.page_no, static_cast<int>(slot)};
                    mark_dirty_slot(batch.table_info, batch.page_info, rid);
                    batch.page_info->active_meta_count_.fetch_add(1, std::memory_order_relaxed);
                }
                if (was_uncommitted) {
                    batch.page_info->uncommitted_meta_count_.fetch_sub(1, std::memory_order_relaxed);
                }
                if (!was_deleted && is_deleted) {
                    batch.page_info->deleted_count_.fetch_add(1, std::memory_order_relaxed);
                } else if (was_deleted && !is_deleted) {
                    batch.page_info->deleted_count_.fetch_sub(1, std::memory_order_relaxed);
                }
                batch.page_info->SetTupleMeta(slot, TupleMeta{commit_version, is_deleted});
                if (publish_version) {
                    VersionUndoLink *version = batch.page_info->GetMutableVersion(slot);
                    if (version == nullptr) {
                        continue;
                    }
                    version->in_progress_ = false;
                }
            }
            timestamp_t observed = batch.page_info->max_committed_meta_ts_.load(std::memory_order_relaxed);
            while (commit_version > observed &&
                   !batch.page_info->max_committed_meta_ts_.compare_exchange_weak(
                       observed, commit_version, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
            bump_page_visibility_epoch(batch.page_info);
        }

        {
            std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
            txn->set_state(TransactionState::COMMITTED);
            last_commit_ts_.store(commit_version);
            if (txn->watermark_registered()) {
                running_txns_.UpdateCommitTs(commit_version);
                running_txns_.RemoveTxn(txn->get_read_ts());
                txn->set_watermark_registered(false);
            }
        }
    }

    for (auto &write_record : write_set) {
        const std::string tab_name = write_record.GetTableName();
        Rid rid = write_record.GetRid();
        if (write_record.GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_record = write_record.GetRecord();
            auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, sm_manager_->db_.get_table(tab_name));
            for (const auto &binding : bindings) {
                rmdb::record_snapshot_index_retirement(tab_name, *binding.meta, rid, txn->get_commit_ts());
            }
            delete_index_entries(sm_manager_, sm_manager_->db_.get_table(tab_name), tab_name, old_record, rid, txn);
        } else if (write_record.GetWriteType() == WType::UPDATE_TUPLE) {
            if (!write_record.IndexKeysChanged()) {
                continue;
            }
            RmRecord &old_record = write_record.GetRecord();
            auto current = sm_manager_->fhs_.at(tab_name)->get_record(rid, nullptr);
            auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, sm_manager_->db_.get_table(tab_name));
            std::vector<std::string> old_key_scratch(bindings.size());
            std::vector<std::string> new_key_scratch(bindings.size());
            for (size_t i = 0; i < bindings.size(); ++i) {
                const auto &binding = bindings[i];
                old_key_scratch[i].resize(binding.meta->col_tot_len);
                new_key_scratch[i].resize(binding.meta->col_tot_len);
                char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
                char *new_key = rmdb::build_index_key_into(*binding.meta, current->data, rid, &new_key_scratch[i]);
                if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
                    rmdb::record_snapshot_index_retirement(tab_name, *binding.meta, rid, txn->get_commit_ts());
                }
            }
            delete_changed_old_index_entries(sm_manager_, sm_manager_->db_.get_table(tab_name), tab_name,
                                             old_record, *current, rid, txn);
        }
    }

    std::vector<RetiredTuple> retired_tuples;
    retired_tuples.reserve(pending_tuples.size());
    for (const auto &pending : pending_tuples) {
        retired_tuples.push_back(RetiredTuple{pending.tab_name, pending.rid});
    }
    EnqueueRetiredTuples(txn->get_commit_ts(), std::move(retired_tuples));

    txn->clear_write_records();
    if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        txn->clear_serializable_state();
        txn->set_ssi_metadata_released(true);
    }
    release_locks(lock_manager_, txn);
    FinishTransactionAdmission(txn);
    admission_finished = true;
    txn->set_gc_ready(true);
    QueueFinishedTransactionForGc(txn);
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
    bool admission_finished = false;
    ScopeExit completion_guard([&] {
        if (!admission_finished &&
            (txn->get_state() == TransactionState::COMMITTED || txn->get_state() == TransactionState::ABORTED)) {
            FinishTransactionAdmission(txn);
        }
    });
    struct AbortTableCache {
        TabMeta *tab = nullptr;
        RmFileHandle *fh = nullptr;
        std::vector<rmdb::IndexBinding> index_bindings;
    };
    std::unordered_map<std::string, AbortTableCache> table_cache;
    auto get_table_cache = [&](const std::string &tab_name) -> AbortTableCache & {
        auto cache_iter = table_cache.find(tab_name);
        if (cache_iter != table_cache.end()) {
            return cache_iter->second;
        }
        AbortTableCache cache;
        cache.tab = &sm_manager_->db_.get_table(tab_name);
        cache.fh = sm_manager_->fhs_.at(tab_name).get();
        cache.index_bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, *cache.tab);
        auto inserted = table_cache.emplace(tab_name, std::move(cache));
        return inserted.first->second;
    };
    auto &write_set = txn->get_write_set();
    while (!write_set.empty()) {
        WriteRecord write_record = std::move(write_set.back());
        write_set.pop_back();
        const std::string tab_name = write_record.GetTableName();
        auto &table = get_table_cache(tab_name);
        Rid rid = write_record.GetRid();

        if (write_record.GetWriteType() == WType::INSERT_TUPLE) {
            auto inserted = table.fh->get_record(rid, nullptr);
            delete_index_entries_bound(table.index_bindings, *inserted, rid, txn);
            table.fh->delete_record(rid, nullptr);
            UpdateTupleMeta(tab_name, rid, std::nullopt);
            UpdateVersionLink(tab_name, rid, std::nullopt);
        } else if (write_record.GetWriteType() == WType::DELETE_TUPLE) {
            auto version_link = GetVersionLink(tab_name, rid);
            if (version_link.has_value() && version_link->prev_.IsValid()) {
                auto undo_log = GetUndoLog(version_link->prev_);
                UpdateTupleMeta(tab_name, rid, TupleMeta{undo_log.ts_, undo_log.is_deleted_});
                UpdateVersionLink(tab_name, rid, VersionUndoLink::FromOptionalUndoLink(
                    undo_log.prev_version_.IsValid() ? std::optional<UndoLink>(undo_log.prev_version_) : std::nullopt));
            } else {
                UpdateTupleMeta(tab_name, rid, std::nullopt);
                UpdateVersionLink(tab_name, rid, std::nullopt);
            }
        } else if (write_record.GetWriteType() == WType::UPDATE_TUPLE) {
            RmRecord &old_record = write_record.GetRecord();
            if (write_record.IndexKeysChanged()) {
                auto current = table.fh->get_record(rid, nullptr);
                delete_changed_new_index_entries_bound(table.index_bindings, old_record, *current, rid, txn);
            }
            table.fh->update_record(rid, old_record.data, nullptr);
            auto version_link = GetVersionLink(tab_name, rid);
            if (version_link.has_value() && version_link->prev_.IsValid()) {
                auto undo_log = GetUndoLog(version_link->prev_);
                UpdateTupleMeta(tab_name, rid, TupleMeta{undo_log.ts_, undo_log.is_deleted_});
                UpdateVersionLink(tab_name, rid, VersionUndoLink::FromOptionalUndoLink(
                    undo_log.prev_version_.IsValid() ? std::optional<UndoLink>(undo_log.prev_version_) : std::nullopt));
            } else {
                UpdateTupleMeta(tab_name, rid, std::nullopt);
                UpdateVersionLink(tab_name, rid, std::nullopt);
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
        txn->set_commit_ts(next_timestamp_++);
        txn->set_state(TransactionState::ABORTED);
        if (txn->watermark_registered()) {
            running_txns_.UpdateCommitTs(last_commit_ts_.load());
            running_txns_.RemoveTxn(txn->get_read_ts());
            txn->set_watermark_registered(false);
        }
        for (auto &entry : txn_map) {
            if (entry.second != nullptr && entry.second != txn) {
                entry.second->remove_rw_dependency(txn->get_transaction_id());
            }
        }
        txn->clear_serializable_state();
        txn->set_ssi_metadata_released(true);
    }
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t abort_lsn = log_manager->add_log_to_buffer(&log_record);
        log_manager->flush_log_to_disk_until_group(abort_lsn + log_record.log_tot_len_ - 1);
    }
    release_locks(lock_manager_, txn);
    FinishTransactionAdmission(txn);
    admission_finished = true;
    txn->set_gc_ready(true);
    QueueFinishedTransactionForGc(txn);
}

void TransactionManager::PhysicalizeCommittedDeletes() {
    if (sm_manager_ == nullptr) {
        return;
    }

    std::vector<std::pair<std::string, std::shared_ptr<TableVersionInfo>>> tables;
    {
        std::shared_lock<std::shared_mutex> version_lock(version_info_mutex_);
        tables.reserve(version_info_.size());
        for (const auto &table_entry : version_info_) {
            if (table_entry.second != nullptr) {
                tables.push_back(table_entry);
            }
        }
    }

    std::vector<std::pair<std::string, Rid>> deletes;
    for (const auto &table_entry : tables) {
        const auto &tab_name = table_entry.first;
        const auto &table_info = table_entry.second;
        std::shared_lock<std::shared_mutex> table_lock(table_info->mutex_);
        for (const auto &page_entry : table_info->pages_) {
            auto page_info = page_entry.second;
            if (page_info == nullptr) {
                continue;
            }
            std::shared_lock<std::shared_mutex> page_lock(page_info->mutex_);
            for (slot_offset_t slot = 0; slot < page_info->slot_count_; ++slot) {
                const TupleMeta *meta = page_info->GetTupleMeta(slot);
                if (meta != nullptr && meta->is_deleted_ && meta->ts_ < TXN_START_ID) {
                    deletes.push_back({tab_name, Rid{page_entry.first, static_cast<int>(slot)}});
                }
            }
        }
    }

    for (const auto &entry : deletes) {
        const std::string &tab_name = entry.first;
        Rid rid = entry.second;
        auto fh_iter = sm_manager_->fhs_.find(tab_name);
        if (fh_iter == sm_manager_->fhs_.end() || fh_iter->second == nullptr) {
            throw InternalError("Cannot physicalize DELETE for unopened table " + tab_name);
        }
        if (fh_iter->second->is_record(rid)) {
            fh_iter->second->delete_record(rid, nullptr);
        }
        UpdateTupleMeta(tab_name, rid, std::nullopt);
        UpdateVersionLink(tab_name, rid, std::nullopt);
    }
}

void delete_changed_old_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                                      const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                      Transaction *txn) {
    auto bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    delete_changed_old_index_entries_bound(bindings, old_record, new_record, rid, txn);
}

void delete_changed_old_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn) {
    std::vector<std::string> old_key_scratch(bindings.size());
    std::vector<std::string> new_key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        old_key_scratch[i].resize(binding.meta->col_tot_len);
        new_key_scratch[i].resize(binding.meta->col_tot_len);
        char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
        char *new_key = rmdb::build_index_key_into(*binding.meta, new_record.data, rid, &new_key_scratch[i]);
        if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
            binding.ih->delete_entry(old_key, txn);
        }
    }
}

void delete_changed_new_index_entries(SmManager *sm_manager, const TabMeta &tab, const std::string &tab_name,
                                      const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                      Transaction *txn) {
    auto bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    delete_changed_new_index_entries_bound(bindings, old_record, new_record, rid, txn);
}

void delete_changed_new_index_entries_bound(const std::vector<rmdb::IndexBinding> &bindings,
                                            const RmRecord &old_record, const RmRecord &new_record, const Rid &rid,
                                            Transaction *txn) {
    std::vector<std::string> old_key_scratch(bindings.size());
    std::vector<std::string> new_key_scratch(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        old_key_scratch[i].resize(binding.meta->col_tot_len);
        new_key_scratch[i].resize(binding.meta->col_tot_len);
        char *old_key = rmdb::build_index_key_into(*binding.meta, old_record.data, rid, &old_key_scratch[i]);
        char *new_key = rmdb::build_index_key_into(*binding.meta, new_record.data, rid, &new_key_scratch[i]);
        if (memcmp(old_key, new_key, binding.meta->col_tot_len) != 0) {
            binding.ih->delete_entry(new_key, txn);
        }
    }
}

void TransactionManager::capture_snapshot(Transaction *txn) {
    if (txn == nullptr || sm_manager_ == nullptr) return;
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
}
