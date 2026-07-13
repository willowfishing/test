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
#include "errors.h"
#include <cstring>

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rid, Context *context) const
{
    // 检查页号合法性
    if (rid.page_no < RM_FIRST_RECORD_PAGE || rid.page_no >= file_hdr_.num_pages)
    {
        // PageNotExistError 的构造需要 table name 和 page_no，传入空表名避免依赖额外成员
        throw PageNotExistError(std::string(), rid.page_no);
    }

    // 获取 page handle（会 pin 页面）
    RmPageHandle page_handle = const_cast<RmFileHandle *>(this)->fetch_page_handle(rid.page_no);

    // 检查 slot 是否存在记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no))
    {
        // 释放 page（unpin）
        const_cast<RmFileHandle *>(this)->release_page_handle(page_handle);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 拷贝记录到 RmRecord（构造时分配大小）
    std::unique_ptr<RmRecord> rec = std::make_unique<RmRecord>(file_hdr_.record_size);
    char *src = page_handle.get_slot(rid.slot_no);
    rec->SetData(src);

    // 释放 page（unpin）
    const_cast<RmFileHandle *>(this)->release_page_handle(page_handle);
    return rec;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char *buf, Context *context)
{
    // 获取一个有空闲slot的page handle（会 pin 页面）
    RmPageHandle page_handle = create_page_handle();

    // 在该页面中找第一个空闲 slot
    int max_slot = file_hdr_.num_records_per_page;
    int found_slot = -1;
    for (int i = 0; i < max_slot; ++i)
    {
        if (!Bitmap::is_set(page_handle.bitmap, i))
        {
            found_slot = i;
            break;
        }
    }
    if (found_slot == -1)
    {
        // 理论上不会发生，因为 create_page_handle 保证有空位
        release_page_handle(page_handle);
        throw InternalError("No free slot found after create_page_handle");
    }

    // 将 buf 写入 slot
    char *dest = page_handle.get_slot(found_slot);
    memcpy(dest, buf, file_hdr_.record_size);

    // 更新 bitmap 与页头计数
    Bitmap::set(page_handle.bitmap, found_slot);
    page_handle.page_hdr->num_records++;

    // 如果该页已满，需要更新 file_hdr_.first_free_page_no 指向下一个有空页
    if (page_handle.page_hdr->num_records >= file_hdr_.num_records_per_page)
    {
        // 从 free list 中移除：file_hdr_.first_free_page_no 应该指向下一空闲页面
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = -1;
        // 将 file header 写回磁盘（保持元数据一致）
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    }

    // 标记页面为脏并释放（unpin）
    Rid result{page_handle.page->get_page_id().page_no, found_slot};
    release_page_handle(page_handle);
    return result;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid &rid, char *buf)
{
    // 获取目标页面句柄
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 写入数据并更新 bitmap / 页头
    char *dest = page_handle.get_slot(rid.slot_no);
    memcpy(dest, buf, file_hdr_.record_size);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no))
    {
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;
    }

    // 标记为脏并释放
    page_handle.page->set_dirty(true);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置)
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid &rid, Context *context)
{
    // 获取指定页面
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 如果 slot 本来就没有记录，直接释放并抛异常
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no))
    {
        release_page_handle(page_handle);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 清除 bitmap 标记并更新页头计数
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;

    // 如果页面从满变为非满，需要把该页加入 file_hdr_ 的空闲链表
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1)
    {
        // 加入 free list
        page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
        file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
        // 将 file header 写回磁盘
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    }

    // 标记脏并释放
    release_page_handle(page_handle);
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context)
{
    // 获取页面
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 检查该 slot 上是否存在记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no))
    {
        release_page_handle(page_handle);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 直接覆盖数据
    char *dest = page_handle.get_slot(rid.slot_no);
    memcpy(dest, buf, file_hdr_.record_size);

    // 标记为脏并释放
    release_page_handle(page_handle);
}

/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const
{
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages)
    {
        throw PageNotExistError(std::string(), page_no);
    }
    PageId pid;
    pid.fd = fd_;
    pid.page_no = page_no;
    Page *page = buffer_pool_manager_->fetch_page(pid);
    if (page == nullptr)
    {
        throw PageNotExistError(std::string(), page_no);
    }
    // 构造并返回 page handle（页面已被 pin）
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle()
{
    // 准备 PageId，告诉缓冲池在哪个文件上分配新页
    PageId pid;
    pid.fd = fd_;
    pid.page_no = INVALID_PAGE_ID;

    Page *page = buffer_pool_manager_->new_page(&pid);
    if (page == nullptr)
    {
        throw InternalError("create_new_page_handle: cannot allocate new page");
    }

    // 初始化页面（页头、bitmap、slots）
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = -1;
    page_handle.page_hdr->num_records = 0;

    // 清空 bitmap 与 slot 区域
    memset(page_handle.bitmap, 0, file_hdr_.bitmap_size);
    memset(page_handle.slots, 0, file_hdr_.record_size * file_hdr_.num_records_per_page);

    // 更新文件头：num_pages 以及把该页加入空闲链表（作为首个空闲页）
    int new_page_no = page->get_page_id().page_no;
    if (file_hdr_.num_pages <= new_page_no)
    {
        file_hdr_.num_pages = new_page_no + 1;
    }
    // 将新页加入 free list head（新页有空闲）
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = new_page_no;

    // 将 file header 写回磁盘
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));

    // 标记页面为脏（初始化后需要持久化）
    page->set_dirty(true);
    // new_page 返回的帧默认已 pin，调用者负责 unpin
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle()
{
    // 如果没有空闲页则创建新的页面
    if (file_hdr_.first_free_page_no == -1)
    {
        return create_new_page_handle();
    }

    // 否则直接取首个空闲页
    int page_no = file_hdr_.first_free_page_no;
    RmPageHandle page_handle = fetch_page_handle(page_no);

    // 从空闲链表中移除（file header 的 first_free_page_no 指向该页的 next）
    file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    page_handle.page_hdr->next_free_page_no = -1;

    // 将 file header 写回磁盘以保持一致
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));

    return page_handle;
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle &page_handle)
{
    // 将页面 unpin，并在必要时将其加入 file_hdr_ 的空闲链表
    PageId pid = page_handle.page->get_page_id();

    // 如果页有空闲 slot 且不在 free list 中，则将其加入 free list
    if (page_handle.page_hdr->num_records < file_hdr_.num_records_per_page)
    {
        // 避免重复加入：如果 next_free_page_no != -1 时认为已经在链表中
        if (page_handle.page_hdr->next_free_page_no == -1)
        {
            page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
            file_hdr_.first_free_page_no = pid.page_no;
            // 写回文件头保证持久化元数据
            disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        }
    }

    // unpin 页面（标记是否为脏由 page_handle.page->is_dirty_ 决定）
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), page_handle.page->is_dirty());
}