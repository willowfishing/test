/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    // 从第一个记录页开始查找
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    // 找到第一个有效记录
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Bitmap::next_bit() searches the open interval (curr, max_n).  Keep
    // slot_no at -1 when entering a new page so the first candidate is slot 0.
    // The old implementation incremented slot_no and then passed slot_no - 1;
    // after a page transition that became -2 and could return a phantom slot
    // -1, causing page-header bytes to be interpreted as a tuple.
    while (rid_.page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        const int next_slot = Bitmap::next_bit(
            true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page,
            rid_.slot_no);
        file_handle_->buffer_pool_manager_->unpin_page(
            page_handle.page->get_page_id(), false);

        if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_.slot_no = next_slot;
            return;
        }
        ++rid_.page_no;
        rid_.slot_no = -1;
    }
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 当page_no超出文件中的页面数量时，表示已到达文件末尾
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
