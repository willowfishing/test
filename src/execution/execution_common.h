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
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <optional>


#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
#include "common/context.h"
#include "common/common.h"

inline auto ReconstructTuple(const TabMeta *schema, const RmRecord &base_tuple, const TupleMeta &base_meta,
                             const std::vector<UndoLog> &undo_logs) -> std::optional<RmRecord> {
    auto apply_undo_log = [&](const UndoLog &undo_log, RmRecord *record, TupleMeta *meta) {
        if (undo_log.tuple_test_ != nullptr) {
            *record = *undo_log.tuple_test_;
        } else if (schema != nullptr && !undo_log.modified_fields_.empty()) {
            size_t value_idx = 0;
            for (size_t col_idx = 0; col_idx < undo_log.modified_fields_.size() && col_idx < schema->cols.size();
                 ++col_idx) {
                if (!undo_log.modified_fields_[col_idx]) {
                    continue;
                }
                if (value_idx >= undo_log.tuple_.size()) {
                    break;
                }
                const auto &col = schema->cols[col_idx];
                const auto &val = undo_log.tuple_[value_idx++];
                if (val.raw == nullptr) {
                    continue;
                }
                memcpy(record->data + col.offset, val.raw->data, col.len);
            }
        }
        meta->ts_ = undo_log.ts_;
        meta->is_deleted_ = undo_log.is_deleted_;
    };

    TupleMeta meta = base_meta;
    RmRecord record = base_tuple;
    for (const auto &undo_log : undo_logs) {
        apply_undo_log(undo_log, &record, &meta);
    }
    if (meta.is_deleted_) {
        return std::nullopt;
    }
    return record;
}

inline auto IsWriteWriteConflict(timestamp_t tuple_ts, Transaction *txn) -> bool {
    if (txn == nullptr || tuple_ts == INVALID_TS) {
        return false;
    }
    if (tuple_ts >= TXN_START_ID) {
        txn_id_t owner = tuple_ts - TXN_START_ID;
        return owner != txn->get_transaction_id();
    }
    return tuple_ts > txn->get_read_ts();
}

inline bool UseMvccReadVisibility(Transaction *txn) {
    if (txn == nullptr) {
        return false;
    }
    return txn->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION ||
           txn->get_isolation_level() == IsolationLevel::SERIALIZABLE;
}

inline bool IsExplicitReadCommittedTxn(Context *context) {
    return context != nullptr && context->txn_ != nullptr && context->txn_->get_txn_mode() &&
           context->txn_->get_isolation_level() == IsolationLevel::READ_COMMITTED;
}

inline bool HasRequiredColumn(const std::vector<TabCol> &required_cols, const std::string &visible_name,
                              const std::string &col_name) {
    return std::any_of(required_cols.begin(), required_cols.end(), [&](const TabCol &col) {
        return col.tab_name == visible_name && col.col_name == col_name;
    });
}

inline bool HasEqualityValueCondition(const std::vector<Condition> &conds, const std::string &visible_name,
                                      const std::string &col_name) {
    return std::any_of(conds.begin(), conds.end(), [&](const Condition &cond) {
        return cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
               cond.lhs_col.tab_name == visible_name && cond.lhs_col.col_name == col_name;
    });
}

inline size_t CountEqualityValueConditions(const std::vector<Condition> &conds, const std::string &visible_name) {
    size_t count = 0;
    for (const auto &cond : conds) {
        if (cond.is_rhs_val && cond.op == OP_EQ && cond.rhs_val.raw != nullptr &&
            cond.lhs_col.tab_name == visible_name && !cond.lhs_col.col_name.empty()) {
            count++;
        }
    }
    return count;
}

inline bool HasProjectedColumnForVisibleName(const std::vector<TabCol> &required_cols,
                                             const std::string &visible_name) {
    return std::any_of(required_cols.begin(), required_cols.end(), [&](const TabCol &col) {
        return col.tab_name == visible_name || col.tab_name.empty();
    });
}

inline bool ColumnNameLooksIdentifierLike(const std::string &name) {
    return name == "id" || (name.size() >= 3 && name.compare(name.size() - 3, 3, "_id") == 0);
}

inline size_t CountProjectedColumnsForVisibleName(const std::vector<TabCol> &required_cols,
                                                  const std::string &visible_name) {
    size_t count = 0;
    for (const auto &col : required_cols) {
        if (!col.col_name.empty() && (col.tab_name == visible_name || col.tab_name.empty())) {
            count++;
        }
    }
    return count;
}

inline const ColMeta *FindVisibleColumnMeta(const std::vector<ColMeta> &cols, const std::string &visible_name,
                                            const std::string &col_name) {
    for (const auto &col : cols) {
        if (col.name == col_name && (col.tab_name == visible_name || col.tab_name.empty())) {
            return &col;
        }
    }
    for (const auto &col : cols) {
        if (col.name == col_name) {
            return &col;
        }
    }
    return nullptr;
}

inline bool HasProjectedAllocatorLikeColumn(const std::vector<TabCol> &required_cols,
                                            const std::vector<ColMeta> &visible_cols,
                                            const std::string &visible_name,
                                            const std::vector<Condition> &conds) {
    for (const auto &col : required_cols) {
        if (col.col_name.empty() || (col.tab_name != visible_name && !col.tab_name.empty())) {
            continue;
        }
        const ColMeta *meta = FindVisibleColumnMeta(visible_cols, visible_name, col.col_name);
        if (meta == nullptr || meta->type != TYPE_INT || !ColumnNameLooksIdentifierLike(meta->name)) {
            continue;
        }
        if (HasEqualityValueCondition(conds, visible_name, meta->name)) {
            continue;
        }
        return true;
    }
    return false;
}

inline bool HasProjectedFloatColumn(const std::vector<TabCol> &required_cols,
                                    const std::vector<ColMeta> &visible_cols,
                                    const std::string &visible_name,
                                    const std::vector<Condition> &conds) {
    for (const auto &col : required_cols) {
        if (col.col_name.empty() || (col.tab_name != visible_name && !col.tab_name.empty())) {
            continue;
        }
        const ColMeta *meta = FindVisibleColumnMeta(visible_cols, visible_name, col.col_name);
        if (meta == nullptr || meta->type != TYPE_FLOAT) {
            continue;
        }
        if (HasEqualityValueCondition(conds, visible_name, meta->name)) {
            continue;
        }
        return true;
    }
    return false;
}

inline bool ShouldTrackReadModifyWritePointRead(Context *context, const std::string &visible_name,
                                                const std::vector<TabCol> &required_cols,
                                                const std::vector<Condition> &conds) {
    if (!IsExplicitReadCommittedTxn(context)) {
        return false;
    }
    if (required_cols.empty() || !HasProjectedColumnForVisibleName(required_cols, visible_name)) {
        return false;
    }
    return CountEqualityValueConditions(conds, visible_name) >= 2;
}

inline bool ShouldEarlyLockPointRead(Context *context, const std::string &visible_name,
                                     const std::vector<TabCol> &required_cols,
                                     const std::vector<Condition> &conds,
                                     const std::vector<ColMeta> &visible_cols) {
    if (!IsExplicitReadCommittedTxn(context)) {
        return false;
    }
    if (CountEqualityValueConditions(conds, visible_name) < 2 ||
        CountProjectedColumnsForVisibleName(required_cols, visible_name) != 2) {
        return false;
    }
    return HasProjectedAllocatorLikeColumn(required_cols, visible_cols, visible_name, conds) &&
           HasProjectedFloatColumn(required_cols, visible_cols, visible_name, conds);
}

inline void LockRecordForEarlyReadOrAbort(Context *context, const Rid &rid, int table_fd) {
    if (context == nullptr || context->lock_mgr_ == nullptr || context->txn_ == nullptr) {
        return;
    }
    if (!context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, table_fd)) {
        throw TransactionAbortException(context->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
}

inline bool IsReadCommittedSlotVisible(TransactionManager *txn_mgr, Transaction *txn, const std::string &tab_name,
                                       const Rid &rid) {
    if (txn_mgr == nullptr) {
        return true;
    }
    auto meta = txn_mgr->GetTupleMeta(tab_name, rid);
    if (!meta.has_value() || !meta->is_deleted_) {
        return true;
    }
    if (meta->ts_ >= TXN_START_ID) {
        txn_id_t owner = meta->ts_ - TXN_START_ID;
        return txn == nullptr || owner != txn->get_transaction_id();
    }
    return false;
}

class ReadCommittedVisibilityCursor {
   public:
    ReadCommittedVisibilityCursor() = default;

    ReadCommittedVisibilityCursor(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name)
        : txn_mgr_(txn_mgr), txn_(txn), tab_name_(std::move(tab_name)) {
        bind_table_info();
    }

    void bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name) {
        txn_mgr_ = txn_mgr;
        txn_ = txn;
        tab_name_ = std::move(tab_name);
        bind_table_info();
        reset();
    }

    void reset() {
        current_page_no_ = RM_NO_PAGE;
        page_info_ = nullptr;
    }

    bool visible(const Rid &rid) {
        if (txn_mgr_ == nullptr) {
            return true;
        }
        if (rid.page_no != current_page_no_) {
            current_page_no_ = rid.page_no;
            page_info_ = txn_mgr_->GetPageVersionInfoOnTableRaw(table_info_, rid.page_no);
        }
        if (page_info_ == nullptr) {
            current_page_no_ = RM_NO_PAGE;
            return true;
        }
        if (page_info_->deleted_count_.load(std::memory_order_relaxed) <= 0) {
            return true;
        }
        std::shared_lock<std::shared_mutex> page_lock(page_info_->mutex_);
        const TupleMeta *meta = rid.slot_no < 0
                                    ? nullptr
                                    : page_info_->GetTupleMeta(static_cast<slot_offset_t>(rid.slot_no));
        if (meta == nullptr || !meta->is_deleted_) {
            return true;
        }
        if (meta->ts_ >= TXN_START_ID) {
            txn_id_t owner = meta->ts_ - TXN_START_ID;
            return txn_ == nullptr || owner != txn_->get_transaction_id();
        }
        return false;
    }

   private:
    void bind_table_info() {
        table_info_ = txn_mgr_ == nullptr ? nullptr : txn_mgr_->GetOrCreateTableVersionInfo(tab_name_);
    }

    TransactionManager *txn_mgr_ = nullptr;
    Transaction *txn_ = nullptr;
    std::string tab_name_;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info_;
    page_id_t current_page_no_ = RM_NO_PAGE;
    TransactionManager::PageVersionInfo *page_info_ = nullptr;
};

class ReadCommittedIndexEntryCursor {
   public:
    ReadCommittedIndexEntryCursor() = default;

    ReadCommittedIndexEntryCursor(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name)
        : txn_mgr_(txn_mgr), txn_(txn), tab_name_(std::move(tab_name)) {
        bind_table_info();
        reset();
    }

    void bind(TransactionManager *txn_mgr, Transaction *txn, std::string tab_name) {
        txn_mgr_ = txn_mgr;
        txn_ = txn;
        tab_name_ = std::move(tab_name);
        bind_table_info();
        reset();
    }

    void reset() {
        current_page_no_ = RM_NO_PAGE;
        page_info_ = nullptr;
        current_cache_ = nullptr;
        current_page_clean_visible_ = false;
        last_tuple_hint_.Reset();
        ++cache_generation_;
        if (cache_generation_ == 0) {
            for (auto &entry : page_cache_) {
                entry.generation = 0;
            }
            cache_generation_ = 1;
        }
    }

    TransactionManager::ReadCommittedIndexEntryState classify(const Rid &rid) {
        last_tuple_hint_.Reset();
        if (txn_mgr_ == nullptr) {
            return TransactionManager::ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        if (rid.page_no != current_page_no_ || current_cache_ == nullptr) {
            current_page_no_ = rid.page_no;
            current_cache_ = lookup_page_info(rid.page_no);
            page_info_ = current_cache_ == nullptr ? nullptr : current_cache_->page_info;
        } else if (page_info_ == nullptr && table_info_ != nullptr) {
            bool page_may_need_info = table_info_->tracks_exact_dirty_page(rid.page_no)
                                          ? table_info_->is_page_dirty_exact(rid.page_no)
                                          : table_info_->maybe_has_page_version_info(rid.page_no);
            if (page_may_need_info) {
                current_cache_ = lookup_page_info(rid.page_no);
                page_info_ = current_cache_ == nullptr ? nullptr : current_cache_->page_info;
            }
        }
        if (page_info_ == nullptr || current_cache_ == nullptr) {
            current_page_clean_visible_ = true;
            return TransactionManager::ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        if (rid.slot_no >= 0 && page_info_->IsSlotClean(static_cast<slot_offset_t>(rid.slot_no))) {
            current_page_clean_visible_ = false;
            return TransactionManager::ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        current_page_clean_visible_ = page_clean_visible(*current_cache_);
        if (current_page_clean_visible_) {
            return TransactionManager::ReadCommittedIndexEntryState::CURRENT_KEY_VISIBLE;
        }
        if (UseMvccReadVisibility(txn_)) {
            return txn_mgr_->ClassifySnapshotIndexEntryOnPage(page_info_, rid, txn_, &last_tuple_hint_);
        }
        return txn_mgr_->ClassifyReadCommittedIndexEntryOnPage(page_info_, rid, txn_, &last_tuple_hint_);
    }

    bool current_page_clean_visible() const { return current_page_clean_visible_; }

    const TransactionManager::ReadCommittedTupleHint *last_tuple_hint() const {
        return last_tuple_hint_.valid ? &last_tuple_hint_ : nullptr;
    }

    bool current_page_still_clean_visible(page_id_t page_no) {
        if (!current_page_clean_visible_ || page_no != current_page_no_ || current_cache_ == nullptr) {
            return false;
        }
        if (page_info_ == nullptr) {
            if (table_info_ == nullptr) {
                return true;
            }
            if (table_info_->tracks_exact_dirty_page(page_no)) {
                return !table_info_->is_page_dirty_exact(page_no);
            }
            return !table_info_->maybe_has_page_version_info(page_no);
        }
        current_page_clean_visible_ = page_clean_visible(*current_cache_);
        return current_page_clean_visible_;
    }

   private:
    static constexpr size_t kPageCacheSize = 512;

    struct CachedPageInfo {
        uint64_t generation = 0;
        page_id_t page_no = RM_NO_PAGE;
        bool valid = false;
        uint64_t page_map_epoch = 0;
        TransactionManager::PageVersionInfo *page_info = nullptr;
        uint64_t visibility_epoch = 0;
        bool clean_cached = false;
        bool clean_visible = false;
    };

    void bind_table_info() {
        table_info_ = txn_mgr_ == nullptr ? nullptr : txn_mgr_->GetOrCreateTableVersionInfo(tab_name_);
    }

    void clear_cached_page_state(CachedPageInfo &entry) {
        entry.visibility_epoch = 0;
        entry.clean_cached = false;
        entry.clean_visible = false;
    }

    CachedPageInfo *lookup_page_info(page_id_t page_no) {
        const auto slot = static_cast<size_t>(page_no) & (kPageCacheSize - 1);
        auto &entry = page_cache_[slot];
        if (entry.generation == cache_generation_ && entry.valid && entry.page_no == page_no) {
            if (entry.page_info == nullptr) {
                if (table_info_ == nullptr) {
                    return &entry;
                }
                if (table_info_->tracks_exact_dirty_page(page_no)) {
                    if (!table_info_->is_page_dirty_exact(page_no)) {
                        return &entry;
                    }
                } else if (!table_info_->maybe_has_page_version_info(page_no)) {
                    return &entry;
                }
            } else {
                return &entry;
            }
            if (table_info_ != nullptr && !table_info_->tracks_exact_dirty_page(page_no)) {
                uint64_t current_epoch = table_info_->page_map_epoch_.load(std::memory_order_acquire);
                if (entry.page_map_epoch == current_epoch) {
                    return &entry;
                }
            }
        }
        clear_cached_page_state(entry);
        entry.generation = cache_generation_;
        entry.page_no = page_no;
        entry.valid = true;
        entry.page_map_epoch = table_info_ == nullptr
                                   ? 0
                                   : table_info_->page_map_epoch_.load(std::memory_order_acquire);
        entry.page_info = nullptr;
        if (table_info_ == nullptr) {
            return &entry;
        }
        if (table_info_->tracks_exact_dirty_page(page_no)) {
            if (!table_info_->is_page_dirty_exact(page_no)) {
                return &entry;
            }
        } else if (!table_info_->maybe_has_page_version_info(page_no)) {
            return &entry;
        }
        uint64_t page_map_epoch = 0;
        auto *page_info = txn_mgr_->GetPageVersionInfoOnTableRaw(table_info_, page_no, &page_map_epoch);
        entry.page_map_epoch = page_map_epoch;
        entry.page_info = page_info;
        return &entry;
    }

    bool page_clean_visible(CachedPageInfo &entry) {
        if (entry.page_info == nullptr) {
            return true;
        }
        uint64_t epoch_before = entry.page_info->visibility_epoch_.load(std::memory_order_acquire);
        if (entry.clean_cached && entry.visibility_epoch == epoch_before) {
            return entry.clean_visible;
        }
        bool clean = UseMvccReadVisibility(txn_)
                         ? txn_mgr_->IsSnapshotPageCleanVisible(entry.page_info, txn_)
                         : txn_mgr_->IsReadCommittedPageCleanVisible(entry.page_info);
        uint64_t epoch_after = entry.page_info->visibility_epoch_.load(std::memory_order_acquire);
        if (epoch_before != epoch_after) {
            entry.clean_cached = false;
            entry.clean_visible = false;
            entry.visibility_epoch = epoch_after;
            return false;
        }
        entry.clean_cached = true;
        entry.clean_visible = clean;
        entry.visibility_epoch = epoch_after;
        return clean;
    }

    TransactionManager *txn_mgr_ = nullptr;
    Transaction *txn_ = nullptr;
    std::string tab_name_;
    std::shared_ptr<TransactionManager::TableVersionInfo> table_info_;
    page_id_t current_page_no_ = RM_NO_PAGE;
    TransactionManager::PageVersionInfo *page_info_ = nullptr;
    std::array<CachedPageInfo, kPageCacheSize> page_cache_;
    uint64_t cache_generation_ = 1;
    CachedPageInfo *current_cache_ = nullptr;
    bool current_page_clean_visible_ = false;
    TransactionManager::ReadCommittedTupleHint last_tuple_hint_;
};
