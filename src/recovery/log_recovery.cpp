/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"
#include "record/rm_file_handle.h"
#include "record/bitmap.h"
#include <algorithm>
#include <cstring>

/**
 * @description: analyze阶段，扫描日志文件，构建脏页表（DPT）和活跃事务表（ATT）
 */
void RecoveryManager::analyze() {
    log_records_.clear();
    dpt_.clear();
    txn_states_.clear();

    // Check for checkpoint restart file
    const std::string restart_file = "db.restart";
    int log_scan_offset = 0;
    if (disk_manager_->is_file(restart_file)) {
        int fd = disk_manager_->get_file_fd(restart_file);
        char buf[sizeof(int)];
        disk_manager_->read_page(fd, 0, buf, sizeof(int));
        log_scan_offset = *reinterpret_cast<int*>(buf);
    }

    // Check if log file exists and has data
    if (!disk_manager_->is_file(LOG_FILE_NAME)) return;
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;

    int remaining = file_size - log_scan_offset;
    if (remaining <= 0) return;

    char* log_data = new char[remaining];
    int bytes_read = disk_manager_->read_log(log_data, remaining, log_scan_offset);
    if (bytes_read <= 0) { delete[] log_data; return; }

    int pos = 0;
    while (pos < bytes_read) {
        LogType log_type = *reinterpret_cast<LogType*>(log_data + pos + OFFSET_LOG_TYPE);
        uint32_t log_tot_len = *reinterpret_cast<uint32_t*>(log_data + pos + OFFSET_LOG_TOT_LEN);
        lsn_t lsn = *reinterpret_cast<lsn_t*>(log_data + pos + OFFSET_LSN);
        txn_id_t txn_id = *reinterpret_cast<txn_id_t*>(log_data + pos + OFFSET_LOG_TID);
        lsn_t prev_lsn = *reinterpret_cast<lsn_t*>(log_data + pos + OFFSET_PREV_LSN);

        if (log_tot_len <= 0 || log_tot_len > 4096) break;  // safety check

        ParsedLogRecord parsed;
        parsed.log_type_ = log_type;
        parsed.lsn_ = lsn;
        parsed.prev_lsn_ = prev_lsn;
        parsed.log_tid_ = txn_id;
        parsed.log_tot_len_ = log_tot_len;

        switch (log_type) {
            case LogType::begin:
                txn_states_[txn_id] = "active";
                break;
            case LogType::commit:
                txn_states_[txn_id] = "committed";
                break;
            case LogType::ABORT:
                txn_states_[txn_id] = "aborted";
                break;
            case LogType::INSERT: {
                InsertLogRecord insert_rec;
                insert_rec.deserialize(log_data + pos);
                parsed.record_ = RmRecord(insert_rec.insert_value_);
                parsed.rid_ = insert_rec.rid_;
                if (insert_rec.table_name_)
                    parsed.table_name_ = std::string(insert_rec.table_name_);
                auto key = std::make_pair(parsed.table_name_, parsed.rid_.page_no);
                dpt_[key].push_back(lsn);
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord del_rec;
                del_rec.deserialize(log_data + pos);
                parsed.record_ = RmRecord(del_rec.old_record_);
                parsed.rid_ = del_rec.rid_;
                if (del_rec.table_name_)
                    parsed.table_name_ = std::string(del_rec.table_name_);
                auto key = std::make_pair(parsed.table_name_, parsed.rid_.page_no);
                dpt_[key].push_back(lsn);
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord upd_rec;
                upd_rec.deserialize(log_data + pos);
                parsed.record_ = RmRecord(upd_rec.old_record_);
                parsed.new_record_ = RmRecord(upd_rec.new_record_);
                parsed.rid_ = upd_rec.rid_;
                if (upd_rec.table_name_)
                    parsed.table_name_ = std::string(upd_rec.table_name_);
                auto key = std::make_pair(parsed.table_name_, parsed.rid_.page_no);
                dpt_[key].push_back(lsn);
                break;
            }
        }

        log_records_.push_back(std::move(parsed));
        pos += log_tot_len;
    }

    delete[] log_data;
}

/**
 * @description: 重做所有已提交但可能未落盘的操作
 */
void RecoveryManager::redo() {
    for (auto& rec : log_records_) {
        if (rec.log_type_ != LogType::INSERT &&
            rec.log_type_ != LogType::DELETE &&
            rec.log_type_ != LogType::UPDATE) {
            continue;
        }

        // Only redo committed transactions
        auto state_it = txn_states_.find(rec.log_tid_);
        if (state_it == txn_states_.end() || state_it->second != "committed") continue;

        if (sm_manager_->fhs_.find(rec.table_name_) == sm_manager_->fhs_.end()) continue;

        auto& fh = sm_manager_->fhs_.at(rec.table_name_);

        // Verify page exists in the file
        if (rec.rid_.page_no <= RM_FILE_HDR_PAGE ||
            rec.rid_.page_no >= fh->get_file_hdr().num_pages) {
            continue;
        }

        // Fetch the page to check its LSN
        RmPageHandle ph = fh->fetch_page_handle(rec.rid_.page_no);

        if (ph.page->get_page_lsn() < rec.lsn_) {
            switch (rec.log_type_) {
                case LogType::INSERT: {
                    if (!Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        Bitmap::set(ph.bitmap, rec.rid_.slot_no);
                        memcpy(ph.get_slot(rec.rid_.slot_no), rec.record_.data,
                               fh->get_file_hdr().record_size);
                        ph.page_hdr->num_records++;
                    }
                    break;
                }
                case LogType::DELETE: {
                    if (Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        Bitmap::reset(ph.bitmap, rec.rid_.slot_no);
                        ph.page_hdr->num_records--;
                    }
                    break;
                }
                case LogType::UPDATE: {
                    if (Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        memcpy(ph.get_slot(rec.rid_.slot_no), rec.new_record_.data,
                               fh->get_file_hdr().record_size);
                    }
                    break;
                }
                default: break;
            }
            ph.page->set_page_lsn(rec.lsn_);
            buffer_pool_manager_->mark_dirty(ph.page);
        }

        buffer_pool_manager_->unpin_page(ph.page->get_page_id(), ph.page->is_dirty());
    }
}

/**
 * @description: 回滚所有未完成的事务
 */
void RecoveryManager::undo() {
    // Collect records for uncommitted transactions, sorted by LSN descending
    std::unordered_map<txn_id_t, std::vector<size_t>> txn_record_indices;
    for (size_t i = 0; i < log_records_.size(); i++) {
        auto& rec = log_records_[i];
        if (rec.log_type_ == LogType::begin || rec.log_type_ == LogType::commit || rec.log_type_ == LogType::ABORT)
            continue;
        auto state_it = txn_states_.find(rec.log_tid_);
        if (state_it == txn_states_.end() || state_it->second == "active") {
            txn_record_indices[rec.log_tid_].push_back(i);
        }
    }

    for (auto& [txn_id, indices] : txn_record_indices) {
        // Sort by LSN descending (undo in reverse order)
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return log_records_[a].lsn_ > log_records_[b].lsn_;
        });

        for (size_t idx : indices) {
            auto& rec = log_records_[idx];

            if (sm_manager_->fhs_.find(rec.table_name_) == sm_manager_->fhs_.end()) continue;
            auto& fh = sm_manager_->fhs_.at(rec.table_name_);

            RmPageHandle ph = fh->fetch_page_handle(rec.rid_.page_no);

            switch (rec.log_type_) {
                case LogType::INSERT:
                    // Undo INSERT: delete the inserted record
                    if (Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        Bitmap::reset(ph.bitmap, rec.rid_.slot_no);
                        ph.page_hdr->num_records--;
                    }
                    break;
                case LogType::DELETE:
                    // Undo DELETE: re-insert the old record
                    if (!Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        Bitmap::set(ph.bitmap, rec.rid_.slot_no);
                        memcpy(ph.get_slot(rec.rid_.slot_no), rec.record_.data,
                               fh->get_file_hdr().record_size);
                        ph.page_hdr->num_records++;
                    }
                    break;
                case LogType::UPDATE:
                    // Undo UPDATE: restore old record
                    if (Bitmap::is_set(ph.bitmap, rec.rid_.slot_no)) {
                        memcpy(ph.get_slot(rec.rid_.slot_no), rec.record_.data,
                               fh->get_file_hdr().record_size);
                    }
                    break;
                default: break;
            }
            ph.page->set_page_lsn(rec.lsn_);
            buffer_pool_manager_->mark_dirty(ph.page);
            buffer_pool_manager_->unpin_page(ph.page->get_page_id(), true);
        }
    }
}
