/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

#include "recovery/log_manager.h"

namespace {
constexpr size_t kFetchCacheSize = 256;

struct FetchCacheEntry {
    const BufferPoolManager *owner = nullptr;
    PageId page_id{0, INVALID_PAGE_ID};
    frame_id_t frame_id = INVALID_FRAME_ID;
    uint64_t generation = 0;
};

thread_local FetchCacheEntry fetch_cache[kFetchCacheSize];

size_t fetch_cache_slot(const PageId &page_id) {
    return std::hash<PageId>()(page_id) & (kFetchCacheSize - 1);
}
}  // namespace

BufferAccessClass BufferPoolManager::access_class_from_strategy(BufferAccessStrategy *strategy) {
    return strategy == nullptr ? BufferAccessClass::Default : strategy->cls;
}

bool BufferPoolManager::access_class_uses_ring(BufferAccessClass access_class) const {
    return access_class == BufferAccessClass::BulkRead || access_class == BufferAccessClass::IndexBuild;
}

bool BufferPoolManager::access_class_is_cold(BufferAccessClass access_class) const {
    return access_class == BufferAccessClass::BulkRead || access_class == BufferAccessClass::ColdWrite ||
           access_class == BufferAccessClass::IndexBuild;
}

void BufferPoolManager::set_frame_access_class(frame_id_t frame_id, BufferAccessClass access_class) {
    if (frame_id != INVALID_FRAME_ID && static_cast<size_t>(frame_id) < frame_access_class_.size()) {
        frame_access_class_[frame_id] = static_cast<uint8_t>(access_class);
    }
}

void BufferPoolManager::note_frame_access(frame_id_t frame_id, BufferAccessClass access_class) {
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= frame_access_class_.size() ||
        access_class == BufferAccessClass::BulkRead || access_class == BufferAccessClass::ColdWrite ||
        access_class == BufferAccessClass::IndexBuild) {
        return;
    }
    auto previous_class = static_cast<BufferAccessClass>(frame_access_class_[frame_id]);
    if (access_class == BufferAccessClass::Hot || previous_class != BufferAccessClass::Hot) {
        frame_access_class_[frame_id] = static_cast<uint8_t>(access_class);
    }
}

bool BufferPoolManager::try_reuse_strategy_frame(PageId page_id, BufferAccessStrategy *strategy,
                                                 frame_id_t *frame_id) {
    if (strategy == nullptr || frame_id == nullptr || strategy->ring.empty() ||
        !access_class_uses_ring(strategy->cls)) {
        return false;
    }

    const size_t ring_size = strategy->ring.size();
    size_t start = strategy->hand % ring_size;
    for (size_t offset = 0; offset < ring_size; ++offset) {
        size_t slot = (start + offset) % ring_size;
        frame_id_t candidate = strategy->ring[slot];
        if (candidate == INVALID_FRAME_ID || static_cast<size_t>(candidate) >= pool_size_) {
            continue;
        }

        std::scoped_lock<std::mutex> frame_lock(frame_latches_[candidate]);
        Page *page = &pages_[candidate];
        if (page->is_replacing_ || page->pin_count_ != 0 || page->is_dirty_ ||
            page->id_.page_no == INVALID_PAGE_ID) {
            continue;
        }
        PageId old_page_id = page->id_;
        if (old_page_id == page_id) {
            continue;
        }

        std::unique_lock<std::mutex> shard_lock(page_table_latch_for(old_page_id), std::try_to_lock);
        if (!shard_lock.owns_lock()) {
            continue;
        }
        auto &old_table = page_table_for(old_page_id);
        auto it = old_table.find(old_page_id);
        if (it == old_table.end() || it->second != candidate || page->pin_count_ != 0 || page->is_dirty_ ||
            page->is_replacing_) {
            continue;
        }

        mark_frame_pinned_locked(candidate);
        page->is_replacing_ = true;
        page->generation_++;
        old_table.erase(it);
        clear_dense_frame(old_page_id, candidate);
        *frame_id = candidate;
        strategy->hand = (slot + 1) % ring_size;
        return true;
    }
    return false;
}

void BufferPoolManager::attach_frame_to_strategy_ring(BufferAccessStrategy *strategy, frame_id_t frame_id) {
    if (strategy == nullptr || strategy->ring.empty() || !access_class_uses_ring(strategy->cls) ||
        frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return;
    }
    size_t slot = strategy->hand % strategy->ring.size();
    strategy->ring[slot] = frame_id;
    strategy->hand = (slot + 1) % strategy->ring.size();
}

size_t BufferPoolManager::page_table_shard_for(const PageId &page_id) const {
    return std::hash<PageId>()(page_id) % page_table_shard_count_;
}

std::unordered_map<PageId, frame_id_t, PageIdHash> &BufferPoolManager::page_table_for(const PageId &page_id) {
    return page_table_shards_[page_table_shard_for(page_id)].table;
}

std::mutex &BufferPoolManager::page_table_latch_for(const PageId &page_id) {
    return page_table_shards_[page_table_shard_for(page_id)].latch;
}

frame_id_t BufferPoolManager::frame_id_for_page(Page *page) const {
    if (page == nullptr) {
        return INVALID_FRAME_ID;
    }
    auto base = reinterpret_cast<std::uintptr_t>(pages_);
    auto ptr = reinterpret_cast<std::uintptr_t>(page);
    auto end = base + sizeof(Page) * pool_size_;
    if (ptr < base || ptr >= end) {
        return INVALID_FRAME_ID;
    }
    auto offset = ptr - base;
    if (offset % sizeof(Page) != 0) {
        return INVALID_FRAME_ID;
    }
    return static_cast<frame_id_t>(offset / sizeof(Page));
}

Page *BufferPoolManager::fetch_page_from_cache(PageId page_id, BufferAccessClass access_class) {
    FetchCacheEntry &entry = fetch_cache[fetch_cache_slot(page_id)];
    if (entry.owner != this || !(entry.page_id == page_id) || entry.frame_id == INVALID_FRAME_ID ||
        static_cast<size_t>(entry.frame_id) >= pool_size_) {
        return nullptr;
    }

    std::scoped_lock<std::mutex> frame_lock(frame_latches_[entry.frame_id]);
    Page *page = &pages_[entry.frame_id];
    if (page->is_replacing_ || !(page->id_ == page_id) || page->generation_ != entry.generation) {
        entry = FetchCacheEntry{};
        return nullptr;
    }
    bool was_unpinned = page->pin_count_ == 0;
    page->pin_count_++;
    if (was_unpinned) {
        mark_frame_pinned_locked(entry.frame_id);
    }
    note_frame_access(entry.frame_id, access_class);
    return page;
}

BufferPoolManager::DenseFrameChunk *BufferPoolManager::dense_chunk_for(PageId page_id, bool create) {
    if (page_id.fd < 0 || page_id.fd >= DiskManager::MAX_FD || page_id.page_no < 0 ||
        page_id.page_no >= kDenseFramePageLimit) {
        return nullptr;
    }
    auto &dir = dense_frame_dirs_[page_id.fd];
    const size_t chunk_idx = static_cast<size_t>(page_id.page_no) >> kDenseFrameChunkBits;
    DenseFrameChunk *chunk = dir.chunks[chunk_idx].load(std::memory_order_acquire);
    if (chunk != nullptr || !create) {
        return chunk;
    }
    std::scoped_lock<std::mutex> lock(dir.latch);
    chunk = dir.chunks[chunk_idx].load(std::memory_order_acquire);
    if (chunk == nullptr) {
        chunk = new DenseFrameChunk();
        dir.chunks[chunk_idx].store(chunk, std::memory_order_release);
    }
    return chunk;
}

frame_id_t BufferPoolManager::lookup_dense_frame(PageId page_id) {
    DenseFrameChunk *chunk = dense_chunk_for(page_id, false);
    if (chunk == nullptr) {
        return INVALID_FRAME_ID;
    }
    const size_t slot = static_cast<size_t>(page_id.page_no) & (kDenseFrameChunkSize - 1);
    return chunk->frames[slot].load(std::memory_order_acquire);
}

void BufferPoolManager::publish_dense_frame(PageId page_id, frame_id_t frame_id) {
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return;
    }
    DenseFrameChunk *chunk = dense_chunk_for(page_id, true);
    if (chunk == nullptr) {
        return;
    }
    const size_t slot = static_cast<size_t>(page_id.page_no) & (kDenseFrameChunkSize - 1);
    chunk->frames[slot].store(frame_id, std::memory_order_release);
}

void BufferPoolManager::clear_dense_frame(PageId page_id, frame_id_t expected_frame_id) {
    DenseFrameChunk *chunk = dense_chunk_for(page_id, false);
    if (chunk == nullptr) {
        return;
    }
    const size_t slot = static_cast<size_t>(page_id.page_no) & (kDenseFrameChunkSize - 1);
    if (expected_frame_id == INVALID_FRAME_ID) {
        chunk->frames[slot].store(INVALID_FRAME_ID, std::memory_order_release);
        return;
    }
    frame_id_t current = expected_frame_id;
    chunk->frames[slot].compare_exchange_strong(current, INVALID_FRAME_ID, std::memory_order_acq_rel,
                                                std::memory_order_acquire);
}

Page *BufferPoolManager::fetch_page_from_dense(PageId page_id, BufferAccessClass access_class) {
    frame_id_t frame_id = lookup_dense_frame(page_id);
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return nullptr;
    }

    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    Page *page = &pages_[frame_id];
    if (page->is_replacing_ || !(page->id_ == page_id)) {
        clear_dense_frame(page_id, frame_id);
        return nullptr;
    }
    bool was_unpinned = page->pin_count_ == 0;
    page->pin_count_++;
    if (was_unpinned) {
        mark_frame_pinned_locked(frame_id);
    }
    remember_fetch_cache(page_id, frame_id, page->generation_);
    note_frame_access(frame_id, access_class);
    return page;
}

void BufferPoolManager::remember_fetch_cache(PageId page_id, frame_id_t frame_id, uint64_t generation) {
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return;
    }
    fetch_cache[fetch_cache_slot(page_id)] = FetchCacheEntry{this, page_id, frame_id, generation};
}

void BufferPoolManager::initialize_frame_for_page(Page *page, PageId page_id, bool bump_generation) {
    page->reset_memory();
    page->id_ = page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    if (bump_generation) {
        page->generation_++;
    }
    page->is_replacing_ = true;
}

void BufferPoolManager::mark_frame_pinned_locked(frame_id_t frame_id) {
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return;
    }
    FrameEvictState state = frame_evict_states_[frame_id];
    if (state == FrameEvictState::kInReplacer) {
        replacer_->pin(frame_id);
    }
    frame_evict_states_[frame_id] = FrameEvictState::kPinned;
}

void BufferPoolManager::mark_frame_evictable_locked(frame_id_t frame_id) {
    if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
        return;
    }
    if (frame_evict_states_[frame_id] == FrameEvictState::kPinned) {
        frame_evict_states_[frame_id] = FrameEvictState::kPending;
        if (!pending_evictable_queued_[frame_id]) {
            pending_evictable_queued_[frame_id] = 1;
            std::scoped_lock<std::mutex> pending_lock(pending_evictable_latch_);
            pending_evictable_frames_.push_back(frame_id);
        }
    }
}

void BufferPoolManager::drain_pending_evictable() {
    std::vector<frame_id_t> frames;
    {
        std::scoped_lock<std::mutex> pending_lock(pending_evictable_latch_);
        if (pending_evictable_frames_.empty()) {
            return;
        }
        frames.swap(pending_evictable_frames_);
    }
    for (frame_id_t frame_id : frames) {
        if (frame_id == INVALID_FRAME_ID || static_cast<size_t>(frame_id) >= pool_size_) {
            continue;
        }
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
        pending_evictable_queued_[frame_id] = 0;
        Page *page = &pages_[frame_id];
        if (frame_evict_states_[frame_id] == FrameEvictState::kPending && !page->is_replacing_ &&
            page->pin_count_ == 0 && page->id_.page_no != INVALID_PAGE_ID) {
            auto access_class = static_cast<BufferAccessClass>(frame_access_class_[frame_id]);
            if (access_class_is_cold(access_class)) {
                replacer_->unpin_cold(frame_id);
            } else {
                replacer_->unpin(frame_id);
            }
            frame_evict_states_[frame_id] = FrameEvictState::kInReplacer;
        }
    }
}

void BufferPoolManager::ensure_wal_before_page_flush(Page *page) {
    if (page == nullptr || !page->is_dirty_ || log_manager_ == nullptr) {
        return;
    }
    log_manager_->flush_log_to_disk_until(page->get_page_lsn());
}

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    return find_victim_page(frame_id, nullptr);
}

bool BufferPoolManager::find_victim_page(frame_id_t* frame_id, VictimSource *source) {
    {
        std::scoped_lock<std::mutex> lock(free_list_latch_);
        if (!free_list_.empty()) {
            *frame_id = free_list_.front();
            free_list_.pop_front();
            if (source != nullptr) {
                *source = VictimSource::kFreeList;
            }
            return true;
        }
    }
    drain_pending_evictable();
    if (replacer_->victim(frame_id)) {
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[*frame_id]);
        frame_evict_states_[*frame_id] = FrameEvictState::kPinned;
        if (source != nullptr) {
            *source = VictimSource::kReplacer;
        }
        return true;
    }
    return false;
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    if (page->id_.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            ensure_wal_before_page_flush(page);
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordDirtyFlush();
            }
            page->is_dirty_ = false;
        }
        auto &old_table = page_table_for(page->id_);
        old_table.erase(page->id_);
        clear_dense_frame(page->id_, new_frame_id);
    }
    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    page_table_for(new_page_id)[new_page_id] = new_frame_id;
    publish_dense_frame(new_page_id, new_frame_id);
}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id, BufferAccessStrategy *strategy) {
    BufferAccessClass access_class = access_class_from_strategy(strategy);
    if (Page *cached_page = fetch_page_from_cache(page_id, access_class)) {
        if (perf_stats_ != nullptr) {
            perf_stats_->RecordBufferPoolHit();
        }
        return cached_page;
    }
    if (Page *dense_page = fetch_page_from_dense(page_id, access_class)) {
        if (perf_stats_ != nullptr) {
            perf_stats_->RecordBufferPoolHit();
        }
        return dense_page;
    }

    {
        std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
        auto &table = page_table_for(page_id);
        auto it = table.find(page_id);
        if (it != table.end()) {
            frame_id_t frame_id = it->second;
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            Page *page = &pages_[frame_id];
            bool was_unpinned = page->pin_count_ == 0;
            page->pin_count_++;
            if (was_unpinned) {
                mark_frame_pinned_locked(frame_id);
            }
            remember_fetch_cache(page_id, frame_id, page->generation_);
            note_frame_access(frame_id, access_class);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolHit();
            }
            return page;
        }
    }

    std::scoped_lock slow_lock{slow_path_latch_};
    {
        std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
        auto &table = page_table_for(page_id);
        auto it = table.find(page_id);
        if (it != table.end()) {
            frame_id_t frame_id = it->second;
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            Page *page = &pages_[frame_id];
            bool was_unpinned = page->pin_count_ == 0;
            page->pin_count_++;
            if (was_unpinned) {
                mark_frame_pinned_locked(frame_id);
            }
            remember_fetch_cache(page_id, frame_id, page->generation_);
            note_frame_access(frame_id, access_class);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolHit();
            }
            return page;
        }
    }

    frame_id_t strategy_frame_id = INVALID_FRAME_ID;
    if (try_reuse_strategy_frame(page_id, strategy, &strategy_frame_id)) {
        Page *page = &pages_[strategy_frame_id];
        {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[strategy_frame_id]);
            initialize_frame_for_page(page, page_id, false);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolMiss();
                perf_stats_->RecordBufferPoolEviction();
            }
            disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
            set_frame_access_class(strategy_frame_id, access_class);
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
            page_table_for(page_id)[page_id] = strategy_frame_id;
            page->is_replacing_ = false;
            publish_dense_frame(page_id, strategy_frame_id);
            remember_fetch_cache(page_id, strategy_frame_id, page->generation_);
        }
        return page;
    }

    frame_id_t frame_id;
    VictimSource source;
    while (find_victim_page(&frame_id, &source)) {
        Page *page = &pages_[frame_id];
        if (source == VictimSource::kFreeList) {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            initialize_frame_for_page(page, page_id, true);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolMiss();
            }
            disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
            set_frame_access_class(frame_id, access_class);
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
            page_table_for(page_id)[page_id] = frame_id;
            page->is_replacing_ = false;
            publish_dense_frame(page_id, frame_id);
            remember_fetch_cache(page_id, frame_id, page->generation_);
            attach_frame_to_strategy_ring(strategy, frame_id);
            return page;
        }

        PageId old_page_id = page->id_;
        bool need_flush = false;
        {
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(old_page_id));
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            auto &old_table = page_table_for(old_page_id);
            auto it = old_table.find(old_page_id);
            if (it == old_table.end() || it->second != frame_id || page->pin_count_ != 0) {
                if (page->pin_count_ == 0) {
                    mark_frame_evictable_locked(frame_id);
                }
                continue;
            }
            page->is_replacing_ = true;
            page->generation_++;
            need_flush = page->is_dirty_;
            old_table.erase(it);
            clear_dense_frame(old_page_id, frame_id);
        }

        {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            if (need_flush) {
                ensure_wal_before_page_flush(page);
                disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->data_, PAGE_SIZE);
                if (perf_stats_ != nullptr) {
                    perf_stats_->RecordDirtyFlush();
                }
                page->is_dirty_ = false;
            }
            initialize_frame_for_page(page, page_id, false);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolMiss();
                perf_stats_->RecordBufferPoolEviction();
            }
            disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
            set_frame_access_class(frame_id, access_class);
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
            page_table_for(page_id)[page_id] = frame_id;
            page->is_replacing_ = false;
            publish_dense_frame(page_id, frame_id);
            remember_fetch_cache(page_id, frame_id, page->generation_);
            attach_frame_to_strategy_ring(strategy, frame_id);
        }
        return page;
    }
    if (perf_stats_ != nullptr) {
        perf_stats_->RecordPinFailure();
    }
    return nullptr;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
    auto &table = page_table_for(page_id);
    auto it = table.find(page_id);
    if (it == table.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    Page *page = &pages_[frame_id];
    if (page->pin_count_ <= 0) {
        return false;
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        mark_frame_evictable_locked(frame_id);
    }
    return true;
}

bool BufferPoolManager::unpin_page_fast(Page *page, PageId expected_page_id, bool is_dirty) {
    frame_id_t frame_id = frame_id_for_page(page);
    if (frame_id == INVALID_FRAME_ID) {
        return false;
    }
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    if (page->is_replacing_ || !(page->id_ == expected_page_id) || page->pin_count_ <= 0) {
        return false;
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        mark_frame_evictable_locked(frame_id);
    }
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
    auto &table = page_table_for(page_id);
    auto it = table.find(page_id);
    if (it == table.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    Page *page = &pages_[frame_id];
    bool was_dirty = page->is_dirty_;
    ensure_wal_before_page_flush(page);
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    if (was_dirty && perf_stats_ != nullptr) {
        perf_stats_->RecordDirtyFlush();
    }
    page->is_dirty_ = false;
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id, BufferAccessStrategy *strategy) {
    BufferAccessClass access_class = access_class_from_strategy(strategy);
    std::scoped_lock slow_lock{slow_path_latch_};
    frame_id_t frame_id;
    VictimSource source;
    while (find_victim_page(&frame_id, &source)) {
        Page *page = &pages_[frame_id];
        bool need_flush = false;
        PageId old_page_id = page->id_;

        if (source == VictimSource::kReplacer) {
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(old_page_id));
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            auto &old_table = page_table_for(old_page_id);
            auto it = old_table.find(old_page_id);
            if (it == old_table.end() || it->second != frame_id || page->pin_count_ != 0) {
                if (page->pin_count_ == 0) {
                    mark_frame_evictable_locked(frame_id);
                }
                continue;
            }
            page->is_replacing_ = true;
            page->generation_++;
            need_flush = page->is_dirty_;
            old_table.erase(it);
            clear_dense_frame(old_page_id, frame_id);
        } else {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            page->is_replacing_ = true;
            page->generation_++;
        }

        page_id->page_no = disk_manager_->allocate_page(page_id->fd);
        {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
            if (need_flush) {
                ensure_wal_before_page_flush(page);
                disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->data_, PAGE_SIZE);
                if (perf_stats_ != nullptr) {
                    perf_stats_->RecordDirtyFlush();
                }
                page->is_dirty_ = false;
            }
            initialize_frame_for_page(page, *page_id, false);
            if (source == VictimSource::kReplacer && perf_stats_ != nullptr) {
                perf_stats_->RecordBufferPoolEviction();
            }
            set_frame_access_class(frame_id, access_class);
            std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(*page_id));
            page_table_for(*page_id)[*page_id] = frame_id;
            page->is_replacing_ = false;
            publish_dense_frame(*page_id, frame_id);
            remember_fetch_cache(*page_id, frame_id, page->generation_);
        }
        return page;
    }
    if (perf_stats_ != nullptr) {
        perf_stats_->RecordPinFailure();
    }
    return nullptr;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock slow_lock{slow_path_latch_};
    frame_id_t frame_id = INVALID_FRAME_ID;
    Page *page = nullptr;
    bool need_flush = false;
    {
        std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
        auto &table = page_table_for(page_id);
        auto it = table.find(page_id);
        if (it == table.end()) {
            disk_manager_->deallocate_page(page_id.page_no);
            return true;
        }
        frame_id = it->second;
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
        page = &pages_[frame_id];
        if (page->pin_count_ != 0) {
            return false;
        }
        mark_frame_pinned_locked(frame_id);
        page->is_replacing_ = true;
        page->generation_++;
        need_flush = page->is_dirty_;
        table.erase(it);
        clear_dense_frame(page_id, frame_id);
    }
    {
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
        if (need_flush) {
            ensure_wal_before_page_flush(page);
            disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordDirtyFlush();
            }
        }
        page->reset_memory();
        page->id_ = PageId{0, INVALID_PAGE_ID};
        page->is_dirty_ = false;
        page->pin_count_ = 0;
        page->is_replacing_ = false;
    }
    {
        std::scoped_lock<std::mutex> free_lock(free_list_latch_);
        free_list_.push_back(frame_id);
    }
    disk_manager_->deallocate_page(page_id.page_no);
    return true;
}

/**
 * @description: 将buffer_pool中的脏页写回到磁盘
 * @param {int} fd 文件句柄，负数表示所有文件
 * @return 实际写回的脏页数量
 */
size_t BufferPoolManager::flush_all_pages(int fd) {
    std::vector<FlushCandidate> candidates;
    lsn_t max_page_lsn = INVALID_LSN;

    for (size_t shard_idx = 0; shard_idx < page_table_shard_count_; ++shard_idx) {
        std::scoped_lock<std::mutex> shard_lock(page_table_shards_[shard_idx].latch);
        for (auto &entry : page_table_shards_[shard_idx].table) {
            if (fd < 0 || entry.first.fd == fd) {
                frame_id_t frame_id = entry.second;
                std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
                Page *page = &pages_[frame_id];
                if (page->id_ == entry.first && page->is_dirty_) {
                    lsn_t page_lsn = page->get_page_lsn();
                    candidates.push_back(FlushCandidate{entry.first, frame_id, page_lsn});
                    if (page_lsn > max_page_lsn) {
                        max_page_lsn = page_lsn;
                    }
                }
            }
        }
    }

    if (log_manager_ != nullptr && max_page_lsn != INVALID_LSN) {
        log_manager_->flush_log_to_disk_until(max_page_lsn);
    }

    size_t flushed_pages = 0;
    for (const auto &candidate : candidates) {
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[candidate.frame_id]);
        Page *page = &pages_[candidate.frame_id];
        if (page->id_ == candidate.page_id && page->is_dirty_) {
            disk_manager_->write_page(candidate.page_id.fd, candidate.page_id.page_no, page->data_, PAGE_SIZE);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordDirtyFlush();
            }
            page->is_dirty_ = false;
            ++flushed_pages;
        }
    }
    return flushed_pages;
}

size_t BufferPoolManager::flush_unpinned_pages_batch(size_t max_pages, size_t max_frames,
                                                     size_t *next_frame, bool *pass_complete) {
    if (next_frame == nullptr || pass_complete == nullptr || max_pages == 0 || max_frames == 0 ||
        pool_size_ == 0) {
        return 0;
    }

    *pass_complete = false;
    size_t frame = *next_frame < pool_size_ ? *next_frame : 0;
    size_t scanned = 0;
    std::vector<FlushCandidate> candidates;
    candidates.reserve(std::min(max_pages, max_frames));
    lsn_t max_page_lsn = INVALID_LSN;
    while (scanned < max_frames && candidates.size() < max_pages) {
        {
            std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame]);
            Page *page = &pages_[frame];
            if (!page->is_replacing_ && page->pin_count_ == 0 && page->id_.page_no != INVALID_PAGE_ID &&
                page->is_dirty_) {
                lsn_t page_lsn = page->get_page_lsn();
                candidates.push_back(FlushCandidate{page->id_, static_cast<frame_id_t>(frame), page_lsn});
                if (page_lsn > max_page_lsn) {
                    max_page_lsn = page_lsn;
                }
            }
        }

        ++scanned;
        ++frame;
        if (frame == pool_size_) {
            frame = 0;
            *pass_complete = true;
            break;
        }
    }
    *next_frame = frame;

    if (log_manager_ != nullptr && max_page_lsn != INVALID_LSN) {
        log_manager_->flush_log_to_disk_until(max_page_lsn);
        disk_manager_->sync_log();
    }

    size_t flushed = 0;
    for (const auto &candidate : candidates) {
        std::scoped_lock<std::mutex> frame_lock(frame_latches_[candidate.frame_id]);
        Page *page = &pages_[candidate.frame_id];
        if (page->id_ == candidate.page_id && !page->is_replacing_ && page->pin_count_ == 0 &&
            page->is_dirty_ && page->get_page_lsn() == candidate.page_lsn) {
            disk_manager_->write_page(candidate.page_id.fd, candidate.page_id.page_no, page->data_, PAGE_SIZE);
            if (perf_stats_ != nullptr) {
                perf_stats_->RecordDirtyFlush();
            }
            page->is_dirty_ = false;
            ++flushed;
        }
    }
    return flushed;
}

bool BufferPoolManager::set_page_lsn(PageId page_id, lsn_t page_lsn) {
    std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
    auto &table = page_table_for(page_id);
    auto it = table.find(page_id);
    if (it == table.end()) {
        return false;
    }
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[it->second]);
    Page *page = &pages_[it->second];
    page->set_page_lsn(page_lsn);
    return true;
}

bool BufferPoolManager::finalize_page_write(PageId page_id, lsn_t page_lsn, bool is_dirty) {
    std::scoped_lock<std::mutex> shard_lock(page_table_latch_for(page_id));
    auto &table = page_table_for(page_id);
    auto it = table.find(page_id);
    if (it == table.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    Page *page = &pages_[frame_id];
    page->set_page_lsn(page_lsn);
    if (page->pin_count_ <= 0) {
        return false;
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        mark_frame_evictable_locked(frame_id);
    }
    return true;
}

bool BufferPoolManager::finalize_page_write_fast(Page *page, PageId expected_page_id, lsn_t page_lsn, bool is_dirty) {
    frame_id_t frame_id = frame_id_for_page(page);
    if (frame_id == INVALID_FRAME_ID) {
        return false;
    }
    std::scoped_lock<std::mutex> frame_lock(frame_latches_[frame_id]);
    if (page->is_replacing_ || !(page->id_ == expected_page_id) || page->pin_count_ <= 0) {
        return false;
    }
    page->set_page_lsn(page_lsn);
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        mark_frame_evictable_locked(frame_id);
    }
    return true;
}
