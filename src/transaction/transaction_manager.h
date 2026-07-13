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
#include <chrono>
#include <cstdint>
#include <array>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <optional>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

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

class RmRecordPageCursor;
class TransactionManager;

class TransactionDrainGuard {
   public:
    TransactionDrainGuard() = default;
    ~TransactionDrainGuard();

    TransactionDrainGuard(const TransactionDrainGuard &) = delete;
    TransactionDrainGuard &operator=(const TransactionDrainGuard &) = delete;

    TransactionDrainGuard(TransactionDrainGuard &&other) noexcept;
    TransactionDrainGuard &operator=(TransactionDrainGuard &&other) noexcept;

    explicit operator bool() const { return manager_ != nullptr; }

   private:
    friend class TransactionManager;
    explicit TransactionDrainGuard(TransactionManager *manager) : manager_(manager) {}
    void reset();

    TransactionManager *manager_{nullptr};
};

class TransactionManager{
public:
    enum class UniqueKeyConflictResult { NONE, ABORT, FAILURE };
    enum class ReadCommittedIndexEntryState { CURRENT_KEY_VISIBLE, INVISIBLE, NEEDS_HEAP };
    struct PageVersionInfo;
    struct TableVersionInfo;
    struct ReadCommittedTupleHint {
        bool valid = false;
        page_id_t page_no = INVALID_PAGE_ID;
        int slot_no = -1;
        PageVersionInfo *page_info = nullptr;
        uint64_t visibility_epoch = 0;
        bool has_meta = false;
        TupleMeta meta{0, false};
        bool has_version_link = false;
        VersionUndoLink version_link{};

        void Reset() {
            valid = false;
            page_no = INVALID_PAGE_ID;
            slot_no = -1;
            page_info = nullptr;
            visibility_epoch = 0;
            has_meta = false;
            meta = TupleMeta{0, false};
            has_version_link = false;
            version_link = VersionUndoLink{};
        }

        bool Matches(const Rid &rid) const {
            return valid && page_no == rid.page_no && slot_no == rid.slot_no && page_info != nullptr;
        }
    };

    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                             ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }
    
    ~TransactionManager() = default;

    Transaction* begin(Transaction* txn, LogManager* log_manager, IsolationLevel isolation_level);

    void commit(Transaction* txn, LogManager* log_manager);
    void capture_snapshot(Transaction* txn);

    void abort(Transaction* txn, LogManager* log_manager);

    void PhysicalizeCommittedDeletes();

    bool UpdateTupleMeta(const std::string &tab_name, Rid rid, std::optional<TupleMeta> meta,
                         std::function<bool(std::optional<TupleMeta>)> &&check = nullptr);

    std::optional<TupleMeta> GetTupleMeta(const std::string &tab_name, Rid rid);

    TupleMeta GetTupleMetaOrDefault(const std::string &tab_name, Rid rid);

    std::shared_ptr<PageVersionInfo> GetPageVersionInfo(const std::string &tab_name, page_id_t page_no);
    std::shared_ptr<TableVersionInfo> GetTableVersionInfo(const std::string &tab_name);
    std::shared_ptr<TableVersionInfo> GetOrCreateTableVersionInfo(const std::string &tab_name);
    std::shared_ptr<PageVersionInfo> GetPageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no);
    std::shared_ptr<PageVersionInfo> GetPageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, uint64_t *page_map_epoch);
    PageVersionInfo *GetPageVersionInfoOnTableRaw(
        const std::shared_ptr<TableVersionInfo> &table_info, page_id_t page_no, uint64_t *page_map_epoch = nullptr);

    std::optional<RmRecord> GetVisibleTuple(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                            TupleMeta *visible_meta = nullptr);

    bool GetVisibleTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn, RmRecord *out_record,
                            TupleMeta *visible_meta = nullptr);

    bool GetReadCommittedTupleInto(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                   RmRecord *out_record, TupleMeta *visible_meta = nullptr,
                                   RmRecordPageCursor *page_cursor = nullptr,
                                   const ReadCommittedTupleHint *hint = nullptr);

    ReadCommittedIndexEntryState ClassifyReadCommittedIndexEntry(const std::string &tab_name, const Rid &rid,
                                                                 Transaction *txn);

    ReadCommittedIndexEntryState ClassifyReadCommittedIndexEntryOnPage(
        const std::shared_ptr<PageVersionInfo> &page_info, const Rid &rid, Transaction *txn);
    ReadCommittedIndexEntryState ClassifyReadCommittedIndexEntryOnPage(
        PageVersionInfo *page_info, const Rid &rid, Transaction *txn,
        ReadCommittedTupleHint *hint = nullptr);
    ReadCommittedIndexEntryState ClassifyReadCommittedIndexEntryState(std::optional<TupleMeta> meta,
                                                                      bool has_version_link,
                                                                      Transaction *txn) const;
    ReadCommittedIndexEntryState ClassifyReadCommittedIndexEntryState(bool has_meta,
                                                                      const TupleMeta &meta,
                                                                      bool has_version_link,
                                                                      Transaction *txn) const;
    ReadCommittedIndexEntryState ClassifySnapshotIndexEntryOnPage(
        PageVersionInfo *page_info, const Rid &rid, Transaction *txn,
        ReadCommittedTupleHint *hint = nullptr);
    ReadCommittedIndexEntryState ClassifySnapshotIndexEntryState(bool has_meta,
                                                                  const TupleMeta &meta,
                                                                  bool has_version_link,
                                                                  Transaction *txn) const;
    bool IsReadCommittedPageCleanVisible(const std::shared_ptr<PageVersionInfo> &page_info) const;
    bool IsReadCommittedPageCleanVisible(const PageVersionInfo *page_info) const;
    bool IsSnapshotPageCleanVisible(const PageVersionInfo *page_info, Transaction *txn) const;

    void EnsureWriteConflictFree(Transaction *txn, const std::string &tab_name, const Rid &rid);

    void EnsureWriteConflictFree(Transaction *txn, const TupleMeta &current_meta);

    void EnsureKeyConflictFree(Transaction *txn, const std::string &tab_name, const TabMeta &tab,
                               const RmRecord &record, const Rid *self_rid = nullptr);

    UniqueKeyConflictResult ClassifyUniqueIndexConflict(Transaction *txn, const std::string &tab_name,
                                                        const IndexMeta &index, const char *key,
                                                        const std::vector<Rid> &matches,
                                                        const Rid *self_rid = nullptr);

    static void record_serializable_read(Transaction* txn, const std::string &tab_name, const Rid &rid);

    static void record_serializable_predicate_read(Transaction* txn, const std::string &tab_name,
                                                   const std::vector<ColMeta> &cols,
                                                   const std::vector<Condition> &conds);

    static void record_serializable_write(Transaction* txn, const std::string &tab_name, const Rid &rid,
                                          const RmRecord *old_record, const RmRecord *new_record,
                                          const std::vector<ColMeta> *cols);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager* get_lock_manager() { return lock_manager_; }

    std::vector<CheckpointTxnInfo> CollectActiveTxnCheckpointInfo(Transaction *exclude_txn = nullptr);

    TransactionDrainGuard BlockNewTransactionsAndWait();

    TransactionDrainGuard TryBlockNewTransactionsIfIdle();

    TransactionDrainGuard BlockNewTransactionsAndWaitFor(std::chrono::milliseconds timeout);

    size_t ActiveTransactionCount() const;

    std::shared_lock<std::shared_mutex> EnterStatementExecution() {
        return std::shared_lock<std::shared_mutex>(checkpoint_latch_);
    }

    std::unique_lock<std::shared_mutex> EnterCheckpointExecution() {
        return std::unique_lock<std::shared_mutex>(checkpoint_latch_);
    }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */    
    Transaction* get_transaction(txn_id_t txn_id) {
        if(txn_id == INVALID_TXN_ID) return nullptr;
        
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        auto txn_iter = TransactionManager::txn_map.find(txn_id);
        assert(txn_iter != TransactionManager::txn_map.end());
        auto *res = txn_iter->second;
        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    static std::unordered_map<txn_id_t, Transaction *> txn_map;     // 全局事务表，存放事务ID与事务对象的映射关系
    static std::shared_mutex txn_map_mutex_;
    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /**
    * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
    * 在更新之前，将调用 `check` 函数以确保有效性。
    */
    bool UpdateUndoLink(const std::string &tab_name, Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(const std::string &tab_name, Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    void InstallTupleVersion(const std::string &tab_name, Rid rid, const VersionUndoLink &version,
                             const TupleMeta &meta);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(const std::string &tab_name, Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(const std::string &tab_name, Rid rid);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** Append an undo log while retaining any cross-transaction predecessor reference. */
    UndoLink AppendUndoLog(Transaction *txn, UndoLog log);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();

    struct PageVersionInfo {
        std::shared_mutex mutex_;
        std::atomic<int> active_meta_count_{0};
        std::atomic<int> uncommitted_meta_count_{0};
        std::atomic<int> deleted_count_{0};
        std::atomic<int> version_link_count_{0};
        std::atomic<timestamp_t> max_committed_meta_ts_{0};
        std::atomic<uint64_t> visibility_epoch_{1};
        std::atomic<uint32_t> dirty_slot_count_{0};
        uint32_t slot_count_{0};
        uint32_t slot_word_count_{0};
        std::unique_ptr<std::atomic<uint64_t>[]> dirty_slot_words_;
        uint32_t dirty_slot_word_count_{0};
        std::unique_ptr<uint64_t[]> tuple_meta_valid_words_;
        std::unique_ptr<TupleMeta[]> tuple_meta_values_;
        std::unique_ptr<uint64_t[]> version_valid_words_;
        std::unique_ptr<VersionUndoLink[]> version_values_;

        void InitDirtySlots(uint32_t slot_count) {
            if (slot_count == 0 || dirty_slot_words_ != nullptr) {
                return;
            }
            uint32_t word_count = (slot_count + 63) / 64;
            auto words = std::make_unique<std::atomic<uint64_t>[]>(word_count);
            for (uint32_t i = 0; i < word_count; ++i) {
                words[i].store(0, std::memory_order_relaxed);
            }
            slot_count_ = slot_count;
            slot_word_count_ = word_count;
            dirty_slot_word_count_ = word_count;
            dirty_slot_words_ = std::move(words);
        }

        bool CanTrackSlot(slot_offset_t slot_no) const {
            return slot_no < slot_count_;
        }

        static uint64_t SlotBit(slot_offset_t slot_no) {
            return uint64_t{1} << (slot_no & 63);
        }

        void EnsureTupleMetaStorage() {
            if (tuple_meta_values_ != nullptr) {
                return;
            }
            tuple_meta_valid_words_ = std::make_unique<uint64_t[]>(slot_word_count_);
            tuple_meta_values_ = std::make_unique<TupleMeta[]>(slot_count_);
        }

        bool HasTupleMeta(slot_offset_t slot_no) const {
            if (!CanTrackSlot(slot_no) || tuple_meta_valid_words_ == nullptr) {
                return false;
            }
            return (tuple_meta_valid_words_[slot_no / 64] & SlotBit(slot_no)) != 0;
        }

        const TupleMeta *GetTupleMeta(slot_offset_t slot_no) const {
            return HasTupleMeta(slot_no) ? &tuple_meta_values_[slot_no] : nullptr;
        }

        TupleMeta *GetMutableTupleMeta(slot_offset_t slot_no) {
            return HasTupleMeta(slot_no) ? &tuple_meta_values_[slot_no] : nullptr;
        }

        bool SetTupleMeta(slot_offset_t slot_no, const TupleMeta &meta) {
            assert(CanTrackSlot(slot_no));
            EnsureTupleMetaStorage();
            uint64_t &word = tuple_meta_valid_words_[slot_no / 64];
            uint64_t bit = SlotBit(slot_no);
            bool existed = (word & bit) != 0;
            tuple_meta_values_[slot_no] = meta;
            word |= bit;
            return existed;
        }

        bool ClearTupleMeta(slot_offset_t slot_no) {
            if (!HasTupleMeta(slot_no)) {
                return false;
            }
            tuple_meta_valid_words_[slot_no / 64] &= ~SlotBit(slot_no);
            return true;
        }

        void ReleaseTupleMetaStorageIfEmpty() {
            if (active_meta_count_.load(std::memory_order_relaxed) == 0) {
                tuple_meta_values_.reset();
                tuple_meta_valid_words_.reset();
            }
        }

        void EnsureVersionStorage() {
            if (version_values_ != nullptr) {
                return;
            }
            version_valid_words_ = std::make_unique<uint64_t[]>(slot_word_count_);
            version_values_ = std::make_unique<VersionUndoLink[]>(slot_count_);
        }

        bool HasVersion(slot_offset_t slot_no) const {
            if (!CanTrackSlot(slot_no) || version_valid_words_ == nullptr) {
                return false;
            }
            return (version_valid_words_[slot_no / 64] & SlotBit(slot_no)) != 0;
        }

        const VersionUndoLink *GetVersion(slot_offset_t slot_no) const {
            return HasVersion(slot_no) ? &version_values_[slot_no] : nullptr;
        }

        VersionUndoLink *GetMutableVersion(slot_offset_t slot_no) {
            return HasVersion(slot_no) ? &version_values_[slot_no] : nullptr;
        }

        bool SetVersion(slot_offset_t slot_no, const VersionUndoLink &version) {
            assert(CanTrackSlot(slot_no));
            EnsureVersionStorage();
            uint64_t &word = version_valid_words_[slot_no / 64];
            uint64_t bit = SlotBit(slot_no);
            bool existed = (word & bit) != 0;
            version_values_[slot_no] = version;
            word |= bit;
            return existed;
        }

        bool ClearVersion(slot_offset_t slot_no) {
            if (!HasVersion(slot_no)) {
                return false;
            }
            version_valid_words_[slot_no / 64] &= ~SlotBit(slot_no);
            return true;
        }

        void ReleaseVersionStorageIfEmpty() {
            if (version_link_count_.load(std::memory_order_relaxed) == 0) {
                version_values_.reset();
                version_valid_words_.reset();
            }
        }

        bool CanTrackDirtySlot(slot_offset_t slot_no) const {
            uint32_t word_idx = static_cast<uint32_t>(slot_no / 64);
            return dirty_slot_words_ != nullptr && word_idx < dirty_slot_word_count_;
        }

        bool MarkDirtySlot(slot_offset_t slot_no) {
            uint32_t word_idx = static_cast<uint32_t>(slot_no / 64);
            if (!CanTrackDirtySlot(slot_no)) {
                return false;
            }
            uint64_t bit = 1ull << (slot_no & 63);
            uint64_t old = dirty_slot_words_[word_idx].fetch_or(bit, std::memory_order_acq_rel);
            if ((old & bit) != 0) {
                return false;
            }
            dirty_slot_count_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }

        bool ClearDirtySlot(slot_offset_t slot_no) {
            uint32_t word_idx = static_cast<uint32_t>(slot_no / 64);
            if (!CanTrackDirtySlot(slot_no)) {
                return false;
            }
            uint64_t bit = 1ull << (slot_no & 63);
            uint64_t old = dirty_slot_words_[word_idx].fetch_and(~bit, std::memory_order_acq_rel);
            if ((old & bit) == 0) {
                return false;
            }
            dirty_slot_count_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }

        bool IsSlotClean(slot_offset_t slot_no) const {
            uint32_t word_idx = static_cast<uint32_t>(slot_no / 64);
            if (dirty_slot_words_ == nullptr || word_idx >= dirty_slot_word_count_) {
                return false;
            }
            uint64_t bit = 1ull << (slot_no & 63);
            return (dirty_slot_words_[word_idx].load(std::memory_order_acquire) & bit) == 0;
        }

        uint32_t DirtySlotCount() const {
            return dirty_slot_count_.load(std::memory_order_acquire);
        }
    };

    struct TableVersionInfo {
        static constexpr page_id_t kExactDirtyPageLimit = 1 << 20;
        static constexpr size_t kExactDirtyPageWordCount = static_cast<size_t>(kExactDirtyPageLimit) / 64;
        static constexpr size_t kDensePageChunkBits = 10;
        static constexpr size_t kDensePageChunkSize = 1 << kDensePageChunkBits;
        static constexpr size_t kDensePageChunkMask = kDensePageChunkSize - 1;
        static constexpr size_t kDensePageChunkCount =
            static_cast<size_t>(kExactDirtyPageLimit) / kDensePageChunkSize;

        struct PageInfoChunk {
            std::array<std::atomic<PageVersionInfo *>, kDensePageChunkSize> pages_{};

            PageInfoChunk() {
                for (auto &page : pages_) {
                    page.store(nullptr, std::memory_order_relaxed);
                }
            }
        };

        TableVersionInfo() {
            for (auto &word : dirty_page_filter_) {
                word.store(0, std::memory_order_relaxed);
            }
            for (auto &word : exact_dirty_pages_) {
                word.store(0, std::memory_order_relaxed);
            }
            for (auto &chunk : page_info_chunks_) {
                chunk.store(nullptr, std::memory_order_relaxed);
            }
        }

        std::shared_mutex mutex_;
        std::atomic<uint64_t> page_map_epoch_{0};
        std::atomic<uint32_t> dirty_page_count_{0};
        std::atomic<uint64_t> dirty_slot_total_{0};
        std::array<std::atomic<uint64_t>, 1024> dirty_page_filter_{};
        std::array<std::atomic<uint64_t>, kExactDirtyPageWordCount> exact_dirty_pages_{};
        std::array<std::atomic<PageInfoChunk *>, kDensePageChunkCount> page_info_chunks_{};
        std::vector<std::unique_ptr<PageInfoChunk>> page_info_chunk_owners_;
        std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> pages_;

        bool maybe_has_page_version_info(page_id_t page_no) const {
            uint64_t hash = static_cast<uint64_t>(static_cast<uint32_t>(page_no)) * 11400714819323198485ull;
            size_t bucket = static_cast<size_t>(hash & (dirty_page_filter_.size() - 1));
            uint64_t bit = 1ull << ((hash >> 10) & 63);
            return (dirty_page_filter_[bucket].load(std::memory_order_acquire) & bit) != 0;
        }

        void mark_page_version_info(page_id_t page_no) {
            uint64_t hash = static_cast<uint64_t>(static_cast<uint32_t>(page_no)) * 11400714819323198485ull;
            size_t bucket = static_cast<size_t>(hash & (dirty_page_filter_.size() - 1));
            uint64_t bit = 1ull << ((hash >> 10) & 63);
            dirty_page_filter_[bucket].fetch_or(bit, std::memory_order_release);
        }

        bool tracks_exact_dirty_page(page_id_t page_no) const {
            return page_no >= 0 && page_no < kExactDirtyPageLimit;
        }

        bool is_page_dirty_exact(page_id_t page_no) const {
            if (!tracks_exact_dirty_page(page_no)) {
                return true;
            }
            size_t word_idx = static_cast<size_t>(page_no) / 64;
            uint64_t bit = 1ull << (static_cast<uint32_t>(page_no) & 63);
            return (exact_dirty_pages_[word_idx].load(std::memory_order_acquire) & bit) != 0;
        }

        bool mark_page_dirty_exact(page_id_t page_no) {
            if (!tracks_exact_dirty_page(page_no)) {
                return false;
            }
            size_t word_idx = static_cast<size_t>(page_no) / 64;
            uint64_t bit = 1ull << (static_cast<uint32_t>(page_no) & 63);
            uint64_t old = exact_dirty_pages_[word_idx].fetch_or(bit, std::memory_order_acq_rel);
            if ((old & bit) != 0) {
                return false;
            }
            dirty_page_count_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }

        bool clear_page_dirty_exact(page_id_t page_no) {
            if (!tracks_exact_dirty_page(page_no)) {
                return false;
            }
            size_t word_idx = static_cast<size_t>(page_no) / 64;
            uint64_t bit = 1ull << (static_cast<uint32_t>(page_no) & 63);
            uint64_t old = exact_dirty_pages_[word_idx].fetch_and(~bit, std::memory_order_acq_rel);
            if ((old & bit) == 0) {
                return false;
            }
            dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }

        PageVersionInfo *lookup_page_info_dense(page_id_t page_no) const {
            if (!tracks_exact_dirty_page(page_no)) {
                return nullptr;
            }
            size_t chunk_idx = static_cast<size_t>(page_no) >> kDensePageChunkBits;
            size_t chunk_offset = static_cast<size_t>(page_no) & kDensePageChunkMask;
            auto *chunk = page_info_chunks_[chunk_idx].load(std::memory_order_acquire);
            if (chunk == nullptr) {
                return nullptr;
            }
            return chunk->pages_[chunk_offset].load(std::memory_order_acquire);
        }

        void publish_page_info_dense(page_id_t page_no, PageVersionInfo *page_info) {
            if (!tracks_exact_dirty_page(page_no) || page_info == nullptr) {
                return;
            }
            size_t chunk_idx = static_cast<size_t>(page_no) >> kDensePageChunkBits;
            size_t chunk_offset = static_cast<size_t>(page_no) & kDensePageChunkMask;
            auto *chunk = page_info_chunks_[chunk_idx].load(std::memory_order_acquire);
            if (chunk == nullptr) {
                auto owner = std::make_unique<PageInfoChunk>();
                chunk = owner.get();
                page_info_chunk_owners_.push_back(std::move(owner));
                page_info_chunks_[chunk_idx].store(chunk, std::memory_order_release);
            }
            chunk->pages_[chunk_offset].store(page_info, std::memory_order_release);
        }

        uint32_t DirtyPageCount() const {
            return dirty_page_count_.load(std::memory_order_acquire);
        }

        uint64_t DirtySlotTotal() const {
            return dirty_slot_total_.load(std::memory_order_acquire);
        }
    };

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储每张表堆中每个元组的先前版本。 */
    std::unordered_map<std::string, std::shared_ptr<TableVersionInfo>> version_info_;
    inline static std::atomic<uint64_t> next_version_cache_id_{1};
    const uint64_t version_cache_id_{next_version_cache_id_.fetch_add(1, std::memory_order_relaxed)};
    std::atomic<uint64_t> version_info_epoch_{1};


private:
    friend class TransactionDrainGuard;

    struct RetiredTuple {
        std::string tab_name;
        Rid rid;
    };

    struct RetiredTransaction {
        timestamp_t commit_ts{INVALID_TS};
        std::vector<RetiredTuple> tuples;
    };

    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};    // 用于分发事务时间戳
    std::shared_mutex checkpoint_latch_;  // 静态检查点期间阻止新语句进入并等待正在执行语句结束
    mutable std::mutex admission_mutex_;
    std::condition_variable admission_cv_;
    bool transaction_admission_blocked_{false};
    size_t active_transaction_count_{0};
    SmManager *sm_manager_;
    LockManager *lock_manager_;

    std::shared_ptr<PageVersionInfo> GetOrCreatePageVersionInfo(const std::string &tab_name, page_id_t page_no);
    std::shared_ptr<PageVersionInfo> GetOrCreatePageVersionInfoOnTable(
        const std::shared_ptr<TableVersionInfo> &table_info, const std::string &tab_name, page_id_t page_no);
    void RetainUndoReference(const UndoLink &link);
    void ReleaseUndoReference(const UndoLink &link);
    void EnqueueRetiredTuples(timestamp_t commit_ts, std::vector<RetiredTuple> tuples);
    bool ReclaimRetiredTuple(timestamp_t commit_ts, const RetiredTuple &retired);
    void QueueFinishedTransactionForGc(Transaction *txn);
    void GarbageCollectFinishedTransactions();
    void FinishTransactionAdmission(Transaction *txn);
    void CancelTransactionAdmissionReservation();
    void ReleaseTransactionDrain();

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC
    std::mutex gc_run_mutex_;
    std::mutex gc_mutex_;
    std::deque<RetiredTransaction> retired_txn_gc_queue_;
    std::atomic<size_t> retired_tuple_count_{0};
    std::deque<txn_id_t> finished_txn_gc_queue_;
    size_t finished_txn_gc_since_last_{0};
};
