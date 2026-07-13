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

#include <assert.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "bitmap.h"
#include "common/context.h"
#include "rm_defs.h"

class RmManager;

/* 对表数据文件中的页面进行封装 */
struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 当前页面所在文件的文件头指针
    Page *page;                 // 页面的实际数据，包括页面存储的数据、元信息等
    RmPageHdr *page_hdr;        // page->data的第一部分，存储页面元信息，指针指向首地址，长度为sizeof(RmPageHdr)
    char *bitmap;               // page->data的第二部分，存储页面的bitmap，指针指向首地址，长度为file_hdr->bitmap_size
    char *slots;                // page->data的第三部分，存储表的记录，指针指向首地址，每个slot的长度为file_hdr->record_size

    RmPageHandle(const RmFileHdr *fhdr_, Page *page_) : file_hdr(fhdr_), page(page_) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page->get_data() + page->OFFSET_PAGE_HDR);
        bitmap = page->get_data() + sizeof(RmPageHdr) + page->OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 返回指定slot_no的slot存储收地址
    char* get_slot(int slot_no) const {
        return slots + slot_no * file_hdr->record_size;  // slots的首地址 + slot个数 * 每个slot的大小(每个record的大小)
    }
};

/* 每个RmFileHandle对应一个表的数据文件，里面有多个page，每个page的数据封装在RmPageHandle中 */
class RmFileHandle {
    friend class RmScan;
    friend class RmManager;

   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    int fd_;        // 打开文件后产生的文件句柄
    RmFileHdr file_hdr_;    // 文件头，维护当前表文件的元数据
    mutable std::shared_mutex file_latch_;

   public:
    RmFileHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
        // 注意：这里从磁盘中读出文件描述符为fd的文件的file_hdr，读到内存中
        // 这里实际就是初始化file_hdr，只不过是从磁盘中读出进行初始化
        // init file_hdr_
        disk_manager_->read_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        // disk_manager管理的fd对应的文件中，设置从file_hdr_.num_pages开始分配page_no
        disk_manager_->set_fd2pageno(fd, file_hdr_.num_pages);
    }

    RmFileHdr get_file_hdr() { return file_hdr_; }
    int GetFd() const { return fd_; }

    void flush_header() {
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char *>(&file_hdr_), sizeof(file_hdr_));
    }

    void flush() {
        flush_header();
        buffer_pool_manager_->flush_all_pages(fd_);
    }

    void rebuild_file_hdr_from_disk();

    /* 判断指定位置上是否已经存在一条记录，通过Bitmap来判断 */
    bool is_record(const Rid &rid) const {
        std::shared_lock<std::shared_mutex> guard(file_latch_);
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        bool exists = rid.slot_no >= 0 && rid.slot_no < file_hdr_.num_records_per_page &&
                      Bitmap::is_set(page_handle.bitmap, rid.slot_no);
        unpin_page_handle_fast(page_handle.page, false);
        return exists;  // page的slot_no位置上是否有record
    }

    std::unique_ptr<RmRecord> get_record(const Rid &rid, Context *context) const;

    bool read_record(const Rid &rid, RmRecord *record, Context *context) const;

    template <typename Fn>
    bool with_record_slot_fast(const Rid &rid, Fn &&fn) const {
        std::shared_lock<std::shared_mutex> guard(file_latch_);
        auto page_handle = fetch_page_handle(rid.page_no);
        if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
            !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            unpin_page_handle_fast(page_handle.page, false);
            return false;
        }
        bool matched = static_cast<bool>(std::forward<Fn>(fn)(page_handle.get_slot(rid.slot_no)));
        unpin_page_handle_fast(page_handle.page, false);
        return matched;
    }

    bool with_record_slot(const Rid &rid, const std::function<bool(const char *slot)> &fn) const;

    Rid insert_record(char *buf, Context *context, PageId *modified_page_id = nullptr, bool defer_unpin = false,
                      Page **modified_page = nullptr, BufferAccessStrategy *strategy = nullptr);

    template <typename FillRecordFn>
    size_t bulk_insert_records(FillRecordFn &&fill_record, BufferAccessStrategy *strategy = nullptr) {
        std::unique_lock<std::shared_mutex> guard(file_latch_);
        std::vector<char> record(static_cast<size_t>(file_hdr_.record_size));
        Page *page = nullptr;
        RmPageHdr *page_hdr = nullptr;
        char *bitmap = nullptr;
        char *slots = nullptr;
        int next_slot = file_hdr_.num_records_per_page;

        auto release_current_page = [&]() {
            if (page != nullptr) {
                unpin_page_handle_fast(page, true);
                page = nullptr;
                page_hdr = nullptr;
                bitmap = nullptr;
                slots = nullptr;
                next_slot = file_hdr_.num_records_per_page;
            }
        };

        auto pin_free_page = [&]() {
            auto page_handle = create_page_handle(strategy);
            page = page_handle.page;
            page_hdr = page_handle.page_hdr;
            bitmap = page_handle.bitmap;
            slots = page_handle.slots;
            next_slot = Bitmap::first_bit(false, bitmap, file_hdr_.num_records_per_page);
            assert(next_slot < file_hdr_.num_records_per_page);
        };

        size_t inserted = 0;
        try {
            while (fill_record(record.data())) {
                if (page == nullptr) {
                    pin_free_page();
                }
                char *slot = slots + next_slot * file_hdr_.record_size;
                Bitmap::set(bitmap, next_slot);
                memcpy(slot, record.data(), file_hdr_.record_size);
                ++page_hdr->num_records;
                ++inserted;

                if (page_hdr->num_records == file_hdr_.num_records_per_page) {
                    file_hdr_.first_free_page_no = page_hdr->next_free_page_no;
                    page_hdr->next_free_page_no = RM_NO_PAGE;
                    release_current_page();
                } else {
                    next_slot = Bitmap::next_bit(false, bitmap, file_hdr_.num_records_per_page, next_slot);
                    assert(next_slot < file_hdr_.num_records_per_page);
                }
            }
            release_current_page();
        } catch (...) {
            release_current_page();
            throw;
        }
        return inserted;
    }

    void insert_record(const Rid &rid, char *buf);

    void delete_record(const Rid &rid, Context *context, PageId *modified_page_id = nullptr,
                       bool defer_unpin = false, Page **modified_page = nullptr);

    void update_record(const Rid &rid, char *buf, Context *context, PageId *modified_page_id = nullptr,
                       bool defer_unpin = false, Page **modified_page = nullptr);

    RmPageHandle create_new_page_handle(BufferAccessStrategy *strategy = nullptr);

    RmPageHandle fetch_page_handle(int page_no, BufferAccessStrategy *strategy = nullptr) const;

    BufferPoolManager *get_buffer_pool_manager() const { return buffer_pool_manager_; }

   private:
    RmPageHandle create_page_handle(BufferAccessStrategy *strategy = nullptr);

    void release_page_handle(RmPageHandle &page_handle);

    void unpin_page_handle_fast(Page *page, bool is_dirty) const {
        PageId page_id = page->get_page_id();
        if (!buffer_pool_manager_->unpin_page_fast(page, page_id, is_dirty)) {
            buffer_pool_manager_->unpin_page(page_id, is_dirty);
        }
    }
};

class RmRecordPageCursor {
   public:
    RmRecordPageCursor() = default;

    explicit RmRecordPageCursor(const RmFileHandle *file_handle) : file_handle_(file_handle) {}

    RmRecordPageCursor(const RmRecordPageCursor &) = delete;
    RmRecordPageCursor &operator=(const RmRecordPageCursor &) = delete;
    RmRecordPageCursor(RmRecordPageCursor &&) = delete;
    RmRecordPageCursor &operator=(RmRecordPageCursor &&) = delete;

    ~RmRecordPageCursor() { reset(); }

    void bind(const RmFileHandle *file_handle) {
        if (file_handle_ == file_handle) {
            return;
        }
        reset();
        file_handle_ = file_handle;
    }

    void reset() {
        if (file_handle_ != nullptr && current_page_no_ != RM_NO_PAGE) {
            PageId page_id{file_handle_->GetFd(), current_page_no_};
            if (!file_handle_->get_buffer_pool_manager()->unpin_page_fast(current_page_, page_id, false)) {
                file_handle_->get_buffer_pool_manager()->unpin_page(page_id, false);
            }
        }
        current_page_ = nullptr;
        current_page_no_ = RM_NO_PAGE;
        file_hdr_ = nullptr;
        bitmap_ = nullptr;
        slots_ = nullptr;
    }

    const char *get_slot(const Rid &rid) {
        if (file_handle_ == nullptr) {
            return nullptr;
        }
        if (rid.page_no != current_page_no_) {
            reset();
            auto handle = file_handle_->fetch_page_handle(rid.page_no);
            current_page_ = handle.page;
            current_page_no_ = rid.page_no;
            file_hdr_ = handle.file_hdr;
            bitmap_ = handle.bitmap;
            slots_ = handle.slots;
        }
        if (rid.slot_no < 0 || rid.slot_no >= file_hdr_->num_records_per_page ||
            !Bitmap::is_set(bitmap_, rid.slot_no)) {
            return nullptr;
        }
        return slots_ + rid.slot_no * file_hdr_->record_size;
    }

    bool read_record(const Rid &rid, RmRecord *record) {
        const char *slot = get_slot(rid);
        if (slot == nullptr) {
            return false;
        }
        record->ResizeAndCopy(slot, file_hdr_->record_size);
        return true;
    }

    template <typename Fn>
    bool with_slot(const Rid &rid, Fn &&fn) {
        const char *slot = get_slot(rid);
        if (slot == nullptr) {
            return false;
        }
        return static_cast<bool>(std::forward<Fn>(fn)(slot));
    }

   private:
    const RmFileHandle *file_handle_ = nullptr;
    Page *current_page_ = nullptr;
    int current_page_no_ = RM_NO_PAGE;
    const RmFileHdr *file_hdr_ = nullptr;
    char *bitmap_ = nullptr;
    char *slots_ = nullptr;
};
