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

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <iostream>
#include "log_defs.h"
#include "common/config.h"
#include "record/rm_defs.h"

/* 日志记录对应操作的类型 */
enum LogType: int {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT,
    CHECKPOINT
};
static std::string LogTypeStr[] = {
    "UPDATE",
    "INSERT",
    "DELETE",
    "BEGIN",
    "COMMIT",
    "ABORT",
    "CHECKPOINT"
};

class LogRecord {
public:
    virtual ~LogRecord() = default;

    LogType log_type_;         /* 日志对应操作的类型 */
    lsn_t lsn_;                /* 当前日志的lsn */
    uint32_t log_tot_len_;     /* 整个日志记录的长度 */
    txn_id_t log_tid_;         /* 创建当前日志的事务ID */
    lsn_t prev_lsn_;           /* 事务创建的前一条日志记录的lsn，用于undo */

    // 把日志记录序列化到dest中
    virtual void serialize (char* dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }
    // 从src中反序列化出一条日志记录
    virtual void deserialize(const char* src) {
        memcpy(&log_type_, src + OFFSET_LOG_TYPE, sizeof(LogType));
        memcpy(&lsn_, src + OFFSET_LSN, sizeof(lsn_t));
        memcpy(&log_tot_len_, src + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        memcpy(&log_tid_, src + OFFSET_LOG_TID, sizeof(txn_id_t));
        memcpy(&prev_lsn_, src + OFFSET_PREV_LSN, sizeof(lsn_t));
    }
    // used for debug
    virtual void format_print() {
        std::cout << "log type in father_function: " << LogTypeStr[log_type_] << "\n";
        std::cout << "Print Log Record:\n";
        std::cout << "log_type_: " << LogTypeStr[log_type_] << "\n";
        std::cout << "lsn: " << lsn_ << "\n";
        std::cout << "log_tot_len: " << log_tot_len_ << "\n";
        std::cout << "log_tid: " << log_tid_ << "\n";
        std::cout << "prev_lsn: " << prev_lsn_ << "\n";
    }
};

class BeginLogRecord: public LogRecord {
public:
    BeginLogRecord() {
        log_type_ = LogType::begin;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() {
        log_tid_ = txn_id;
    }
    // 序列化Begin日志记录到dest中
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    // 从src中反序列化出一条Begin日志记录
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);   
    }
    virtual void format_print() override {
        std::cout << "log type in son_function: " << LogTypeStr[log_type_] << "\n";
        LogRecord::format_print();
    }
};

class CommitLogRecord: public LogRecord {
public:
    CommitLogRecord() {
        log_type_ = LogType::commit;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    CommitLogRecord(txn_id_t txn_id) : CommitLogRecord() {
        log_tid_ = txn_id;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
    }
    void format_print() override {
        printf("commit record\n");
        LogRecord::format_print();
    }
};

class AbortLogRecord: public LogRecord {
public:
    AbortLogRecord() {
        log_type_ = LogType::ABORT;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() {
        log_tid_ = txn_id;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
    }
    void format_print() override {
        printf("abort record\n");
        LogRecord::format_print();
    }
};

struct CheckpointTxnInfo {
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t last_lsn_{INVALID_LSN};
};

class CheckpointLogRecord: public LogRecord {
public:
    CheckpointLogRecord() {
        log_type_ = LogType::CHECKPOINT;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
        active_txn_count_ = 0;
    }
    CheckpointLogRecord(const std::vector<CheckpointTxnInfo>& active_txns) : CheckpointLogRecord() {
        active_txn_count_ = static_cast<int>(active_txns.size());
        active_txns_ = active_txns;
        log_tot_len_ += sizeof(int);
        if (active_txn_count_ > 0) {
            log_tot_len_ += active_txn_count_ * (sizeof(txn_id_t) + sizeof(lsn_t));
        }
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &active_txn_count_, sizeof(int));
        offset += sizeof(int);
        for (int i = 0; i < active_txn_count_; ++i) {
            memcpy(dest + offset, &active_txns_[i].txn_id_, sizeof(txn_id_t));
            offset += sizeof(txn_id_t);
            memcpy(dest + offset, &active_txns_[i].last_lsn_, sizeof(lsn_t));
            offset += sizeof(lsn_t);
        }
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        memcpy(&active_txn_count_, src + offset, sizeof(int));
        offset += sizeof(int);
        active_txns_.resize(active_txn_count_);
        for (int i = 0; i < active_txn_count_; ++i) {
            memcpy(&active_txns_[i].txn_id_, src + offset, sizeof(txn_id_t));
            offset += sizeof(txn_id_t);
            memcpy(&active_txns_[i].last_lsn_, src + offset, sizeof(lsn_t));
            offset += sizeof(lsn_t);
        }
    }
    void format_print() override {
        printf("checkpoint record\n");
        LogRecord::format_print();
        printf("active_txn_count: %d\n", active_txn_count_);
    }
    int active_txn_count_;
    std::vector<CheckpointTxnInfo> active_txns_;   // 检查点时刻活跃事务及其最近日志位置
};

class InsertLogRecord: public LogRecord {
public:
    InsertLogRecord() {
        log_type_ = LogType::INSERT;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    InsertLogRecord(txn_id_t txn_id, RmRecord& insert_value, Rid& rid, const std::string& table_name)
        : InsertLogRecord() {
        log_tid_ = txn_id;
        insert_value_ = insert_value;
        rid_ = rid;
        log_tot_len_ += sizeof(int);
        log_tot_len_ += insert_value_.size;
        log_tot_len_ += sizeof(Rid);
        table_name_ = table_name;
        log_tot_len_ += sizeof(size_t) + table_name_.size();
    }

    // 把insert日志记录序列化到dest中
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &insert_value_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, insert_value_.data, insert_value_.size);
        offset += insert_value_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = table_name_.size();
        memcpy(dest + offset, &table_name_size, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_.data(), table_name_size);
    }
    // 从src中反序列化出一条Insert日志记录
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        int record_size = 0;
        memcpy(&record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        insert_value_.ResizeAndCopy(src + offset, record_size);
        offset += record_size;
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = 0;
        memcpy(&table_name_size, src + offset, sizeof(size_t));
        offset += sizeof(size_t);
        table_name_.assign(src + offset, table_name_size);
    }
    void format_print() override {
        printf("insert record\n");
        LogRecord::format_print();
        printf("insert_value: %s\n", insert_value_.data);
        printf("insert rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_.c_str());
    }

    RmRecord insert_value_;     // 插入的记录
    Rid rid_;                   // 记录插入的位置
    std::string table_name_;    // 插入记录的表名称
};

class DeleteLogRecord: public LogRecord {
public:
    DeleteLogRecord() {
        log_type_ = LogType::DELETE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    DeleteLogRecord(txn_id_t txn_id, RmRecord& old_record, Rid& rid, const std::string& table_name)
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        old_record_ = old_record;
        rid_ = rid;
        log_tot_len_ += sizeof(int) + old_record_.size;
        log_tot_len_ += sizeof(Rid);
        table_name_ = table_name;
        log_tot_len_ += sizeof(size_t) + table_name_.size();
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &old_record_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, old_record_.data, old_record_.size);
        offset += old_record_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = table_name_.size();
        memcpy(dest + offset, &table_name_size, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_.data(), table_name_size);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        int record_size = 0;
        memcpy(&record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        old_record_.ResizeAndCopy(src + offset, record_size);
        offset += record_size;
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = 0;
        memcpy(&table_name_size, src + offset, sizeof(size_t));
        offset += sizeof(size_t);
        table_name_.assign(src + offset, table_name_size);
    }
    void format_print() override {
        printf("delete record\n");
        LogRecord::format_print();
        printf("delete rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_.c_str());
    }
    RmRecord old_record_;       // 被删除的记录（用于undo）
    Rid rid_;                   // 记录的位置
    std::string table_name_;
};

class UpdateLogRecord: public LogRecord {
public:
    UpdateLogRecord() {
        log_type_ = LogType::UPDATE;
        lsn_ = INVALID_LSN;
        log_tot_len_ = LOG_HEADER_SIZE;
        log_tid_ = INVALID_TXN_ID;
        prev_lsn_ = INVALID_LSN;
    }
    UpdateLogRecord(txn_id_t txn_id, RmRecord& old_record, RmRecord& new_record, Rid& rid,
                    const std::string& table_name)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        old_record_ = old_record;
        new_record_ = new_record;
        rid_ = rid;
        log_tot_len_ += sizeof(int) + old_record_.size;
        log_tot_len_ += sizeof(int) + new_record_.size;
        log_tot_len_ += sizeof(Rid);
        table_name_ = table_name;
        log_tot_len_ += sizeof(size_t) + table_name_.size();
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        memcpy(dest + offset, &old_record_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, old_record_.data, old_record_.size);
        offset += old_record_.size;
        memcpy(dest + offset, &new_record_.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, new_record_.data, new_record_.size);
        offset += new_record_.size;
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = table_name_.size();
        memcpy(dest + offset, &table_name_size, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name_.data(), table_name_size);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        int old_record_size = 0;
        memcpy(&old_record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        old_record_.ResizeAndCopy(src + offset, old_record_size);
        offset += old_record_size;
        int new_record_size = 0;
        memcpy(&new_record_size, src + offset, sizeof(int));
        offset += sizeof(int);
        new_record_.ResizeAndCopy(src + offset, new_record_size);
        offset += new_record_size;
        memcpy(&rid_, src + offset, sizeof(Rid));
        offset += sizeof(Rid);
        size_t table_name_size = 0;
        memcpy(&table_name_size, src + offset, sizeof(size_t));
        offset += sizeof(size_t);
        table_name_.assign(src + offset, table_name_size);
    }
    void format_print() override {
        printf("update record\n");
        LogRecord::format_print();
        printf("update rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_.c_str());
    }
    RmRecord old_record_;       // 更新前的记录（用于undo）
    RmRecord new_record_;       // 更新后的记录（用于redo）
    Rid rid_;                   // 记录的位置
    std::string table_name_;
};

/* 日志缓冲区，只有一个buffer，因此需要阻塞地去把日志写入缓冲区中 */

class LogBuffer {
public:
    LogBuffer() { 
        offset_ = 0; 
        memset(buffer_, 0, sizeof(buffer_));
    }

    bool is_full(int append_size) {
        if(offset_ + append_size > LOG_BUFFER_SIZE)
            return true;
        return false;
    }

    void reset() { offset_ = 0; }

    char buffer_[LOG_BUFFER_SIZE+1];
    int offset_;    // 写入log的offset
};

/* 日志管理器，负责把日志写入日志缓冲区，以及把日志缓冲区中的内容写入磁盘中 */
class LogManager {
public:
    LogManager(DiskManager* disk_manager) {
        disk_manager_ = disk_manager;
        persist_lsn_ = INVALID_LSN;
    }

    lsn_t add_log_to_buffer(LogRecord* log_record);
    void flush_log_to_disk();
    void flush_log_to_disk_until(lsn_t target_lsn);
    void flush_log_to_disk_until_group(lsn_t target_lsn);
    lsn_t get_log_file_offset();
    lsn_t get_persist_lsn();
    void reset_log_file_offset(lsn_t log_file_offset);

private:
    void flush_log_to_disk_until_unlocked(std::unique_lock<std::mutex> &lock, lsn_t target_lsn);
    void flush_active_buffer_unlocked(std::unique_lock<std::mutex> &lock);

    std::atomic<lsn_t> global_lsn_{0};  // 全局lsn，递增，用于为每条记录分发lsn
    std::mutex latch_;                  // 用于对log_buffer_的互斥访问
    std::condition_variable flush_cv_;
    std::condition_variable group_flush_cv_;
    LogBuffer log_buffers_[2];          // active buffer接收新日志，flush buffer负责当前写盘
    LogBuffer *active_buffer_{&log_buffers_[0]};
    LogBuffer *flush_buffer_{&log_buffers_[1]};
    lsn_t persist_lsn_;                 // 记录已经持久化到磁盘中的最后一条日志的日志号
    lsn_t active_start_offset_{0};      // active buffer中第一条日志对应的文件偏移
    lsn_t next_log_offset_{0};          // 下一个日志记录写入的文件偏移
    lsn_t flush_start_offset_{0};
    int flush_size_{0};
    bool flush_in_progress_{false};
    bool group_leader_waiting_{false};
    uint64_t append_epoch_{0};
    DiskManager* disk_manager_;
}; 
