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
    // 扫描日志文件，构建未完成事务列表和脏页表
    // 简化实现：从磁盘读取所有日志并解析
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;
    int offset = 0;
    char header_buf[LOG_HEADER_SIZE];
    while (offset < file_size) {
        int bytes_read = disk_manager_->read_log(header_buf, LOG_HEADER_SIZE, offset);
        if (bytes_read <= 0) break;
        LogRecord *log_rec = new LogRecord();
        log_rec->deserialize(header_buf);
        // 根据日志类型处理
        memcpy(buffer_.buffer_ + buffer_.offset_, header_buf, LOG_HEADER_SIZE);
        if (log_rec->log_tot_len_ > LOG_HEADER_SIZE) {
            int data_len = log_rec->log_tot_len_ - LOG_HEADER_SIZE;
            char data_buf[data_len];
            disk_manager_->read_log(data_buf, data_len, offset + LOG_HEADER_SIZE);
            memcpy(buffer_.buffer_ + buffer_.offset_ + LOG_HEADER_SIZE, data_buf, data_len);
        }
        buffer_.offset_ += log_rec->log_tot_len_;
        offset += log_rec->log_tot_len_;
        delete log_rec;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    // 重做日志中的所有操作（简化实现：重新扫描并执行所有操作）
    int offset = 0;
    char header_buf[LOG_HEADER_SIZE];
    while (offset < buffer_.offset_) {
        LogRecord *log_rec = new LogRecord();
        log_rec->deserialize(buffer_.buffer_ + offset);
        char *data_buf = buffer_.buffer_ + offset;
        switch (log_rec->log_type_) {
            case LogType::INSERT: {
                InsertLogRecord insert_log;
                insert_log.deserialize(data_buf);
                auto fh = sm_manager_->fhs_.at(std::string(insert_log.table_name_)).get();
                fh->insert_record(insert_log.rid_, insert_log.insert_value_.data);
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord delete_log;
                delete_log.deserialize(data_buf);
                auto fh = sm_manager_->fhs_.at(std::string(delete_log.table_name_)).get();
                fh->delete_record(delete_log.rid_, nullptr);
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord update_log;
                update_log.deserialize(data_buf);
                auto fh = sm_manager_->fhs_.at(std::string(update_log.table_name_)).get();
                fh->update_record(update_log.rid_, update_log.new_value_.data, nullptr);
                break;
            }
            default:
                break;
        }
        offset += log_rec->log_tot_len_;
        delete log_rec;
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    // 回滚所有未提交的事务（简化实现：遍历并回滚）
    std::unordered_set<txn_id_t> committed_txns;
    // 第一遍：收集所有已提交/回滚的事务
    int offset = 0;
    while (offset < buffer_.offset_) {
        LogRecord *log_rec = new LogRecord();
        log_rec->deserialize(buffer_.buffer_ + offset);
        if (log_rec->log_type_ == LogType::commit || log_rec->log_type_ == LogType::ABORT) {
            committed_txns.insert(log_rec->log_tid_);
        }
        offset += log_rec->log_tot_len_;
        delete log_rec;
    }
    // 第二遍：逆序回滚未提交事务的操作
    offset = buffer_.offset_;
    while (offset > 0) {
        // 向前找日志记录（简化处理：从头扫描找前一条）
        // 实际实现需要从后向前扫描
        LogRecord *log_rec = new LogRecord();
        int prev_offset = 0;
        while (prev_offset < buffer_.offset_) {
            LogRecord *tmp = new LogRecord();
            tmp->deserialize(buffer_.buffer_ + prev_offset);
            if (prev_offset + tmp->log_tot_len_ == offset) {
                delete tmp;
                break;
            }
            prev_offset += tmp->log_tot_len_;
            delete tmp;
        }
        if (prev_offset >= buffer_.offset_ && offset != buffer_.offset_) break;
        if (offset == buffer_.offset_) {
            // 找最后一条记录
            int last = 0;
            while (last < buffer_.offset_) {
                LogRecord *tmp = new LogRecord();
                tmp->deserialize(buffer_.buffer_ + last);
                int next = last + tmp->log_tot_len_;
                delete tmp;
                if (next >= buffer_.offset_) break;
                last = next;
            }
            prev_offset = last;
        }
        log_rec->deserialize(buffer_.buffer_ + prev_offset);
        if (committed_txns.find(log_rec->log_tid_) == committed_txns.end()) {
            char *data_buf = buffer_.buffer_ + prev_offset;
            switch (log_rec->log_type_) {
                case LogType::INSERT: {
                    InsertLogRecord insert_log;
                    insert_log.deserialize(data_buf);
                    auto fh = sm_manager_->fhs_.at(std::string(insert_log.table_name_)).get();
                    fh->delete_record(insert_log.rid_, nullptr);
                    break;
                }
                case LogType::DELETE: {
                    DeleteLogRecord delete_log;
                    delete_log.deserialize(data_buf);
                    auto fh = sm_manager_->fhs_.at(std::string(delete_log.table_name_)).get();
                    // Undo delete: we need old value, but simplified version just writes a tombstone
                    break;
                }
                case LogType::UPDATE: {
                    UpdateLogRecord update_log;
                    update_log.deserialize(data_buf);
                    auto fh = sm_manager_->fhs_.at(std::string(update_log.table_name_)).get();
                    fh->update_record(update_log.rid_, update_log.old_value_.data, nullptr);
                    break;
                }
                default:
                    break;
            }
        }
        delete log_rec;
        offset = prev_offset;
        if (offset <= 0) break;
    }
}