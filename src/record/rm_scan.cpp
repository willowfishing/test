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
#include <algorithm> // for std::max

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(const_cast<RmFileHandle *>(file_handle))
{
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;
    // 定位到第一个合法记录（如果存在）
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next()
{
    // 从当前 rid 的下一个位置开始查找下一个已分配的 slot
    int start_page = std::max(rid_.page_no, RM_FIRST_RECORD_PAGE - 1);
    int num_pages = file_handle_->file_hdr_.num_pages;

    // 从当前页的下一个 slot 开始（如果当前未初始化则从 0 开始）
    int cur_page = start_page;
    int cur_slot = (rid_.slot_no >= 0) ? (rid_.slot_no + 1) : 0;

    // 清标志表示还未找到
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;

    for (; cur_page < num_pages; ++cur_page)
    {
        // 对于第一次循环，如果我们从 RM_FIRST_RECORD_PAGE-1 开始，需要把 cur_slot 置为 0
        if (cur_page == start_page && start_page == RM_FIRST_RECORD_PAGE - 1)
        {
            cur_slot = 0;
        }
        // 获取该页面的 page_handle（该函数会 pin 页面）
        RmPageHandle page_handle = file_handle_->fetch_page_handle(cur_page);
        int max_slot = file_handle_->file_hdr_.num_records_per_page;
        for (int s = cur_slot; s < max_slot; ++s)
        {
            // Bitmap::is_set 用于检查该 slot 是否已被占用
            if (Bitmap::is_set(page_handle.bitmap, s))
            {
                // 找到下一条记录，设置 rid 并释放 page_handle（unpin）
                rid_.page_no = cur_page;
                rid_.slot_no = s;
                // release_page_handle 会执行 unpin，同时在需要时更新空闲链表
                file_handle_->release_page_handle(page_handle);
                return;
            }
        }
        // 本页无可用记录，释放 page 并继续下页
        file_handle_->release_page_handle(page_handle);
        // 下页从 slot 0 开始
        cur_slot = 0;
    }

    // 遍历结束，rid 保持为 RM_NO_PAGE 表示结束
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const
{
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const
{
    return rid_;
}