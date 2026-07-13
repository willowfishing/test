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

#include <unordered_set>

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 检查记录是否存在
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    // 2. 初始化一个指向RmRecord的指针（赋值其内部的data和size）
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    char *slot = page_handle.get_slot(rid.slot_no);
    memcpy(record->data, slot, file_hdr_.record_size);
    // 释放页面
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    // 1. 获取当前未满的page handle
    RmPageHandle page_handle = create_page_handle();
    // 2. 在page handle中找到空闲slot位置
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    const int page_no = page_handle.page->get_page_id().page_no;
    const Rid rid{page_no, slot_no};
    // The page remains pinned until the WAL record is in the log buffer, so
    // no dirty data page can reach disk before its log record.
    if (context != nullptr && context->log_mgr_ != nullptr && context->txn_ != nullptr) {
        RmRecord value(file_hdr_.record_size, buf);
        InsertLogRecord record(context->txn_->get_transaction_id(), value, rid,
                               disk_manager_->get_file_name(fd_), context->txn_->get_prev_lsn());
        const lsn_t lsn = context->log_mgr_->add_log_to_buffer(&record);
        context->txn_->set_prev_lsn(lsn);
    }
    // 3. 将buf复制到空闲slot位置
    char *slot = page_handle.get_slot(slot_no);
    memcpy(slot, buf, file_hdr_.record_size);
    // 更新bitmap
    Bitmap::set(page_handle.bitmap, slot_no);
    // 4. 更新page_handle.page_hdr中的数据结构
    page_handle.page_hdr->num_records++;
    // 注意考虑插入一条记录后页面已满的情况，需要更新file_hdr_.first_free_page_no
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        // 页面已满，将first_free_page_no指向下一个有空闲空间的页面
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }
    // 释放页面（标记为脏页）
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    ensure_page_exists(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
        throw InternalError("Invalid recovery slot number");
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("Cannot restore a record into an occupied slot");
    }

    char *slot = page_handle.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;

    // A deleted tuple from a formerly full page puts that page back into the
    // free-page chain. Restoring the tuple during transaction rollback can
    // make the page full again, so unlink it from that chain as well.
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        const int restored_page_no = rid.page_no;
        const int restored_next = page_handle.page_hdr->next_free_page_no;
        if (file_hdr_.first_free_page_no == restored_page_no) {
            file_hdr_.first_free_page_no = restored_next;
        } else {
            int current_page_no = file_hdr_.first_free_page_no;
            std::unordered_set<int> visited;
            while (current_page_no != RM_NO_PAGE && current_page_no > 0 &&
                   current_page_no < file_hdr_.num_pages &&
                   visited.insert(current_page_no).second) {
                RmPageHandle current = fetch_page_handle(current_page_no);
                if (current.page_hdr->next_free_page_no == restored_page_no) {
                    current.page_hdr->next_free_page_no = restored_next;
                    buffer_pool_manager_->unpin_page(current.page->get_page_id(), true);
                    break;
                }
                const int next_page_no = current.page_hdr->next_free_page_no;
                buffer_pool_manager_->unpin_page(current.page->get_page_id(), false);
                current_page_no = next_page_no;
            }
        }
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 检查记录是否存在
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    if (context != nullptr && context->log_mgr_ != nullptr && context->txn_ != nullptr) {
        RmRecord old_value(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
        DeleteLogRecord record(context->txn_->get_transaction_id(), old_value, rid,
                               disk_manager_->get_file_name(fd_), false,
                               context->txn_->get_prev_lsn());
        const lsn_t lsn = context->log_mgr_->add_log_to_buffer(&record);
        context->txn_->set_prev_lsn(lsn);
    }
    // 记录页面删除前是否已满
    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);
    // 2. 更新page_handle.page_hdr中的数据结构
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    // 注意考虑删除一条记录后页面未满的情况，需要调用release_page_handle()
    if (was_full) {
        release_page_handle(page_handle);
    }
    // 释放页面
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    // 1. 获取指定记录所在的page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 检查记录是否存在
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    // 2. WAL first, then update the in-memory page.
    char *slot = page_handle.get_slot(rid.slot_no);
    if (context != nullptr && context->log_mgr_ != nullptr && context->txn_ != nullptr) {
        RmRecord old_value(file_hdr_.record_size, slot);
        RmRecord new_value(file_hdr_.record_size, buf);
        UpdateLogRecord record(context->txn_->get_transaction_id(), old_value, new_value, rid,
                               disk_manager_->get_file_name(fd_), false, context->txn_->get_prev_lsn());
        const lsn_t lsn = context->log_mgr_->add_log_to_buffer(&record);
        context->txn_->set_prev_lsn(lsn);
    }
    memcpy(slot, buf, file_hdr_.record_size);
    // 释放页面
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}


void RmFileHandle::ensure_page_exists(int page_no) {
    if (page_no < 1) throw PageNotExistError("unknown", page_no);
    while (file_hdr_.num_pages <= page_no) {
        RmPageHandle page_handle = create_new_page_handle();
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    }
}

void RmFileHandle::rebuild_free_page_list() {
    int first_free = RM_NO_PAGE;
    int previous_free = RM_NO_PAGE;
    for (int page_no = 1; page_no < file_hdr_.num_pages; ++page_no) {
        RmPageHandle page_handle = fetch_page_handle(page_no);
        int count = 0;
        for (int slot = 0; slot < file_hdr_.num_records_per_page; ++slot) {
            if (Bitmap::is_set(page_handle.bitmap, slot)) ++count;
        }
        page_handle.page_hdr->num_records = count;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        if (count < file_hdr_.num_records_per_page) {
            if (first_free == RM_NO_PAGE) first_free = page_no;
            if (previous_free != RM_NO_PAGE) {
                RmPageHandle previous = fetch_page_handle(previous_free);
                previous.page_hdr->next_free_page_no = page_no;
                buffer_pool_manager_->unpin_page(previous.page->get_page_id(), true);
            }
            previous_free = page_no;
        }
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    }
    file_hdr_.first_free_page_no = first_free;
    disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
}


void RmFileHandle::repair_free_page_list(const std::vector<int> &affected_pages) {
    std::unordered_set<int> candidates;
    std::unordered_set<int> visited;
    int current = file_hdr_.first_free_page_no;
    while (current != RM_NO_PAGE && current > 0 && current < file_hdr_.num_pages &&
           visited.insert(current).second) {
        RmPageHandle page = fetch_page_handle(current);
        const int next = page.page_hdr->next_free_page_no;
        if (page.page_hdr->num_records < file_hdr_.num_records_per_page) {
            candidates.insert(current);
        }
        buffer_pool_manager_->unpin_page(page.page->get_page_id(), false);
        current = next;
    }

    for (int page_no : affected_pages) {
        if (page_no <= 0 || page_no >= file_hdr_.num_pages) continue;
        RmPageHandle page = fetch_page_handle(page_no);
        if (page.page_hdr->num_records < file_hdr_.num_records_per_page) {
            candidates.insert(page_no);
        } else {
            candidates.erase(page_no);
        }
        page.page_hdr->next_free_page_no = RM_NO_PAGE;
        buffer_pool_manager_->unpin_page(page.page->get_page_id(), true);
    }

    std::vector<int> chain(candidates.begin(), candidates.end());
    std::sort(chain.begin(), chain.end());
    file_hdr_.first_free_page_no = chain.empty() ? RM_NO_PAGE : chain.front();
    for (size_t i = 0; i < chain.size(); ++i) {
        RmPageHandle page = fetch_page_handle(chain[i]);
        page.page_hdr->next_free_page_no =
            (i + 1 < chain.size()) ? chain[i + 1] : RM_NO_PAGE;
        buffer_pool_manager_->unpin_page(page.page->get_page_id(), true);
    }
    disk_manager_->set_fd2pageno(fd_, file_hdr_.num_pages);
}

void RmFileHandle::flush_file_hdr() {
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                              reinterpret_cast<const char *>(&file_hdr_),
                              sizeof(file_hdr_));
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 使用缓冲池获取指定页面，并生成page_handle返回给上层
    // if page_no is invalid, throw PageNotExistError exception
    if (page_no < 0 || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("unknown", page_no);
    }
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw PageNotExistError("unknown", page_no);
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1. 使用缓冲池来创建一个新page
    PageId page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id);
    // 2. 更新page handle中的相关信息
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    page_handle.page_hdr->num_records = 0;
    // 初始化bitmap
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    // 3. 更新file_hdr_
    file_hdr_.num_pages++;
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // 1. 判断file_hdr_中是否还有空闲页
    //     1.1 没有空闲页：使用缓冲池来创建一个新page；可直接调用create_new_page_handle()
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        // No free page, create new one and set it as first free page
        RmPageHandle page_handle = create_new_page_handle();
        file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        return page_handle;
    }
    //     1.2 有空闲页：直接获取第一个空闲页
    // 2. 生成page handle并返回给上层
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    // 当page从已满变成未满，考虑如何更新：
    // 1. page_handle.page_hdr->next_free_page_no
    // 2. file_hdr_.first_free_page_no
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}
