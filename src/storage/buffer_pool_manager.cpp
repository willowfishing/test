#include "buffer_pool_manager.h"

bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->get_data(), PAGE_SIZE);
        page->is_dirty_ = false;
    }
    page_table_.erase(page->id_);
    page->id_ = new_page_id;
    page->reset_memory();
    page_table_[new_page_id] = new_frame_id;
}

Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        page->pin_count_++;
        replacer_->pin(frame_id);
        return page;
    }
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) { return nullptr; }
    Page* victim_page = &pages_[frame_id];
    if (victim_page->is_dirty_) {
        disk_manager_->write_page(victim_page->id_.fd, victim_page->id_.page_no,
                                  victim_page->get_data(), PAGE_SIZE);
    }
    if (victim_page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(victim_page->id_);
    }
    disk_manager_->read_page(page_id.fd, page_id.page_no, victim_page->get_data(), PAGE_SIZE);
    victim_page->id_ = page_id;
    victim_page->is_dirty_ = false;
    victim_page->pin_count_ = 1;
    page_table_[page_id] = frame_id;
    replacer_->pin(frame_id);
    return victim_page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) { return false; }
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    if (page->pin_count_ <= 0) { return false; }
    page->pin_count_--;
    if (page->pin_count_ == 0) { replacer_->unpin(frame_id); }
    if (is_dirty) { page->is_dirty_ = true; }
    return true;
}

bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) { return false; }
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->get_data(), PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) { return nullptr; }
    Page* page = &pages_[frame_id];
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->get_data(), PAGE_SIZE);
    }
    if (page->id_.page_no != INVALID_PAGE_ID) { page_table_.erase(page->id_); }
    page_id_t new_page_no = disk_manager_->allocate_page(page_id->fd);
    *page_id = {page_id->fd, new_page_no};
    page->id_ = *page_id;
    page->reset_memory();
    page->is_dirty_ = false;
    page->pin_count_ = 1;
    page_table_[*page_id] = frame_id;
    replacer_->pin(frame_id);
    return page;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) { return true; }
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    if (page->pin_count_ != 0) { return false; }
    replacer_->pin(frame_id);
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->get_data(), PAGE_SIZE);
    }
    page->id_.page_no = INVALID_PAGE_ID;
    page->is_dirty_ = false;
    page->reset_memory();
    page_table_.erase(it);
    free_list_.push_back(frame_id);
    return true;
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (auto &entry : page_table_) {
        if (entry.first.fd == fd) {
            frame_id_t frame_id = entry.second;
            Page* page = &pages_[frame_id];
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->get_data(), PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
