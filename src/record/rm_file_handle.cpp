/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

namespace {
void unpin_page_handle(BufferPoolManager *buffer_pool_manager, Page *page, bool is_dirty) {
    PageId page_id = page->get_page_id();
    if (!buffer_pool_manager->unpin_page_fast(page, page_id, is_dirty)) {
        buffer_pool_manager->unpin_page(page_id, is_dirty);
    }
}
}  // namespace

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    std::shared_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
    return record;
}

bool RmFileHandle::read_record(const Rid& rid, RmRecord *record, Context* context) const {
    std::shared_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
        return false;
    }
    record->ResizeAndCopy(page_handle.get_slot(rid.slot_no), file_hdr_.record_size);
    unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
    return true;
}

bool RmFileHandle::with_record_slot(const Rid& rid, const std::function<bool(const char *slot)> &fn) const {
    return with_record_slot_fast(rid, fn);
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context, PageId *modified_page_id, bool defer_unpin,
                                Page **modified_page, BufferAccessStrategy *strategy) {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = create_page_handle(strategy);
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    assert(slot_no < file_hdr_.num_records_per_page);
    Bitmap::set(page_handle.bitmap, slot_no);
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    page_handle.page_hdr->num_records++;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    if (modified_page_id != nullptr) {
        *modified_page_id = page_handle.page->get_page_id();
    }
    if (modified_page != nullptr) {
        *modified_page = page_handle.page;
    }
    if (!defer_unpin) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    page_handle.page_hdr->num_records++;
    if (!was_full && page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
            page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        }
    }
    unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context, PageId *modified_page_id, bool defer_unpin,
                                 Page **modified_page) {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    if (was_full) {
        release_page_handle(page_handle);
    }
    if (modified_page_id != nullptr) {
        *modified_page_id = page_handle.page->get_page_id();
    }
    if (modified_page != nullptr) {
        *modified_page = page_handle.page;
    }
    if (!defer_unpin) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context, PageId *modified_page_id,
                                 bool defer_unpin, Page **modified_page) {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    auto page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    if (modified_page_id != nullptr) {
        *modified_page_id = page_handle.page->get_page_id();
    }
    if (modified_page != nullptr) {
        *modified_page = page_handle.page;
    }
    if (!defer_unpin) {
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no, BufferAccessStrategy *strategy) const {
    if (page_no <= RM_FILE_HDR_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no}, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::fetch_page failed");
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle(BufferAccessStrategy *strategy) {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id, strategy);
    if (page == nullptr) {
        throw InternalError("BufferPoolManager::new_page failed");
    }
    file_hdr_.num_pages++;
    auto page_handle = RmPageHandle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    file_hdr_.first_free_page_no = page_id.page_no;
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle(BufferAccessStrategy *strategy) {
    while (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        page_id_t page_no = file_hdr_.first_free_page_no;
        auto page_handle = fetch_page_handle(page_no, strategy);
        if (Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page) <
            file_hdr_.num_records_per_page) {
            return page_handle;
        }

        page_id_t next_free_page_no = page_handle.page_hdr->next_free_page_no;
        if (next_free_page_no == page_no) {
            next_free_page_no = RM_NO_PAGE;
        }
        file_hdr_.first_free_page_no = next_free_page_no;
        page_handle.page_hdr->num_records = file_hdr_.num_records_per_page;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        unpin_page_handle(buffer_pool_manager_, page_handle.page, true);
    }
    return create_new_page_handle(strategy);
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}

void RmFileHandle::rebuild_file_hdr_from_disk() {
    std::unique_lock<std::shared_mutex> guard(file_latch_);
    int64_t file_size = disk_manager_->get_file_size(disk_manager_->get_file_name(fd_));
    int actual_num_pages =
        file_size <= 0 ? 1 : static_cast<int>((file_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (actual_num_pages < 1) {
        actual_num_pages = 1;
    }

    file_hdr_.num_pages = actual_num_pages;
    file_hdr_.first_free_page_no = RM_NO_PAGE;

    for (int page_no = actual_num_pages - 1; page_no >= RM_FIRST_RECORD_PAGE; --page_no) {
        Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
        if (page == nullptr) {
            throw InternalError("BufferPoolManager::fetch_page failed in rebuild_file_hdr_from_disk");
        }
        RmPageHandle page_handle(&file_hdr_, page);
        if (page_handle.page_hdr->num_records < file_hdr_.num_records_per_page) {
            page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
            file_hdr_.first_free_page_no = page_no;
            unpin_page_handle(buffer_pool_manager_, page, true);
        } else {
            page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
            unpin_page_handle(buffer_pool_manager_, page, true);
        }
    }

    disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
}
