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

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    int fd = disk_manager_->open_file(LOG_FILE_NAME);
    if (fd < 0) return;
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { disk_manager_->close_file(fd); return; }
    lseek(fd, 0, SEEK_SET);
    char* log_data = new char[file_size];
    ssize_t bytes_read = read(fd, log_data, file_size);
    disk_manager_->close_file(fd);
    if (bytes_read <= 0) { delete[] log_data; return; }
    int offset = 0;
    while (offset < bytes_read) {
        LogType log_type = *reinterpret_cast<LogType*>(log_data + offset);
        uint32_t log_tot_len = *reinterpret_cast<uint32_t*>(log_data + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t log_tid = *reinterpret_cast<txn_id_t*>(log_data + offset + OFFSET_LOG_TID);
        if (log_tot_len == 0 || offset + log_tot_len > bytes_read) break;
        if (log_type == LogType::commit) {
            committed_txns_.insert(log_tid);
            active_txns_.erase(log_tid);
        } else if (log_type == LogType::begin) {
            active_txns_.insert(log_tid);
        } else if (log_type == LogType::ABORT) {
            active_txns_.erase(log_tid);
        }
        offset += log_tot_len;
    }
    log_data_ = log_data;
    log_data_size_ = bytes_read;
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    if (log_data_ == nullptr || log_data_size_ == 0) return;
    int offset = 0;
    while (offset < log_data_size_) {
        LogType log_type = *reinterpret_cast<LogType*>(log_data_ + offset);
        uint32_t log_tot_len = *reinterpret_cast<uint32_t*>(log_data_ + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t log_tid = *reinterpret_cast<txn_id_t*>(log_data_ + offset + OFFSET_LOG_TID);
        if (log_tot_len == 0 || offset + log_tot_len > log_data_size_) break;
        if (committed_txns_.count(log_tid)) {
            if (log_type == LogType::INSERT) {
                InsertLogRecord record;
                record.deserialize(log_data_ + offset);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->insert_record(record.rid_, record.insert_value_.data);
                }
            } else if (log_type == LogType::DELETE) {
                DeleteLogRecord record;
                record.deserialize(log_data_ + offset);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->delete_record(record.rid_, nullptr);
                }
            } else if (log_type == LogType::UPDATE) {
                UpdateLogRecord record;
                record.deserialize(log_data_ + offset);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->update_record(record.rid_, record.new_value_.data, nullptr);
                }
            }
        }
        offset += log_tot_len;
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    if (log_data_ == nullptr || log_data_size_ == 0) return;
    int offset = log_data_size_;
    while (offset > 0) {
        int record_start = -1;
        for (int i = offset - 4; i >= 0; --i) {
            if (i + OFFSET_LOG_TOT_LEN + (int)sizeof(uint32_t) <= offset) {
                uint32_t check_len = *reinterpret_cast<uint32_t*>(log_data_ + i + OFFSET_LOG_TOT_LEN);
                if (i + check_len == offset) { record_start = i; break; }
            }
        }
        if (record_start < 0) break;
        LogType log_type = *reinterpret_cast<LogType*>(log_data_ + record_start);
        txn_id_t log_tid = *reinterpret_cast<txn_id_t*>(log_data_ + record_start + OFFSET_LOG_TID);
        if (!committed_txns_.count(log_tid)) {
            if (log_type == LogType::INSERT) {
                InsertLogRecord record;
                record.deserialize(log_data_ + record_start);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->delete_record(record.rid_, nullptr);
                }
            } else if (log_type == LogType::DELETE) {
                DeleteLogRecord record;
                record.deserialize(log_data_ + record_start);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->insert_record(record.rid_, record.delete_value_.data);
                }
            } else if (log_type == LogType::UPDATE) {
                UpdateLogRecord record;
                record.deserialize(log_data_ + record_start);
                auto it = sm_manager_->fhs_.find(std::string(record.table_name_));
                if (sm_manager_ != nullptr && it != sm_manager_->fhs_.end()) {
                    it->second->update_record(record.rid_, record.old_value_.data, nullptr);
                }
            }
        }
        offset = record_start;
    }
    delete[] log_data_;
    log_data_ = nullptr;
    log_data_size_ = 0;
}
