/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "log_defs.h"
#include "record/rm_defs.h"

/* 日志记录对应操作的类型。枚举值会持久化到磁盘，已有类型的取值不能改变。 */
enum LogType : int {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT,
    CHECKPOINT
};

inline const char *log_type_name(LogType type) {
    switch (type) {
        case UPDATE: return "UPDATE";
        case INSERT: return "INSERT";
        case DELETE: return "DELETE";
        case begin: return "BEGIN";
        case commit: return "COMMIT";
        case ABORT: return "ABORT";
        case CHECKPOINT: return "CHECKPOINT";
    }
    return "UNKNOWN";
}

class LogRecord {
public:
    virtual ~LogRecord() = default;

    LogType log_type_{begin};
    lsn_t lsn_{INVALID_LSN};
    uint32_t log_tot_len_{LOG_HEADER_SIZE};
    txn_id_t log_tid_{INVALID_TXN_ID};
    lsn_t prev_lsn_{INVALID_LSN};

    virtual void serialize(char *dest) const {
        std::memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        std::memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        std::memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        std::memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        std::memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }

    virtual void deserialize(const char *src) {
        std::memcpy(&log_type_, src + OFFSET_LOG_TYPE, sizeof(LogType));
        std::memcpy(&lsn_, src + OFFSET_LSN, sizeof(lsn_t));
        std::memcpy(&log_tot_len_, src + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        std::memcpy(&log_tid_, src + OFFSET_LOG_TID, sizeof(txn_id_t));
        std::memcpy(&prev_lsn_, src + OFFSET_PREV_LSN, sizeof(lsn_t));
    }

    virtual void format_print() const {
        std::cout << "log_type=" << log_type_name(log_type_) << " lsn=" << lsn_
                  << " len=" << log_tot_len_ << " txn=" << log_tid_
                  << " prev=" << prev_lsn_ << '\n';
    }
};

class BeginLogRecord : public LogRecord {
public:
    BeginLogRecord() { log_type_ = LogType::begin; }
    explicit BeginLogRecord(txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN) : BeginLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
    }
};

class CommitLogRecord : public LogRecord {
public:
    CommitLogRecord() { log_type_ = LogType::commit; }
    explicit CommitLogRecord(txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN) : CommitLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
    }
};

class AbortLogRecord : public LogRecord {
public:
    AbortLogRecord() { log_type_ = LogType::ABORT; }
    explicit AbortLogRecord(txn_id_t txn_id, lsn_t prev_lsn = INVALID_LSN) : AbortLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
    }
};

class InsertLogRecord : public LogRecord {
public:
    InsertLogRecord() { log_type_ = LogType::INSERT; }
    InsertLogRecord(txn_id_t txn_id, const RmRecord &insert_value, const Rid &rid,
                    std::string table_name, lsn_t prev_lsn = INVALID_LSN)
        : InsertLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
        insert_value_ = insert_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(int32_t) + insert_value_.size +
                       sizeof(Rid) + sizeof(uint32_t) + table_name_.size();
    }
    void serialize(char *dest) const override;
    void deserialize(const char *src) override;

    RmRecord insert_value_;
    Rid rid_{};
    std::string table_name_;
};

class DeleteLogRecord : public LogRecord {
public:
    DeleteLogRecord() { log_type_ = LogType::DELETE; }
    DeleteLogRecord(txn_id_t txn_id, const RmRecord &delete_value, const Rid &rid,
                    std::string table_name, bool logical_delete,
                    lsn_t prev_lsn = INVALID_LSN)
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
        delete_value_ = delete_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        logical_delete_ = logical_delete;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(int32_t) + delete_value_.size +
                       sizeof(Rid) + sizeof(uint8_t) + sizeof(uint32_t) + table_name_.size();
    }
    void serialize(char *dest) const override;
    void deserialize(const char *src) override;

    RmRecord delete_value_;
    Rid rid_{};
    std::string table_name_;
    bool logical_delete_{false};
};

class UpdateLogRecord : public LogRecord {
public:
    UpdateLogRecord() { log_type_ = LogType::UPDATE; }
    UpdateLogRecord(txn_id_t txn_id, const RmRecord &old_value, const RmRecord &new_value,
                    const Rid &rid, std::string table_name,
                    bool tombstone_revival = false,
                    lsn_t prev_lsn = INVALID_LSN)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        prev_lsn_ = prev_lsn;
        old_value_ = old_value;
        new_value_ = new_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        tombstone_revival_ = tombstone_revival;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(int32_t) + old_value_.size +
                       sizeof(int32_t) + new_value_.size + sizeof(Rid) + sizeof(uint8_t) +
                       sizeof(uint32_t) + table_name_.size();
    }
    void serialize(char *dest) const override;
    void deserialize(const char *src) override;

    RmRecord old_value_;
    RmRecord new_value_;
    Rid rid_{};
    std::string table_name_;
    bool tombstone_revival_{false};
};

struct CheckpointTombstone {
    std::string table_name;
    Rid rid{};
};

class CheckpointLogRecord : public LogRecord {
public:
    CheckpointLogRecord() {
        log_type_ = LogType::CHECKPOINT;
        log_tid_ = INVALID_TXN_ID;
    }
    explicit CheckpointLogRecord(std::vector<txn_id_t> active_txns,
                                 std::vector<CheckpointTombstone> tombstones)
        : CheckpointLogRecord() {
        active_txns_ = std::move(active_txns);
        tombstones_ = std::move(tombstones);
        uint64_t total = LOG_HEADER_SIZE + sizeof(uint32_t) +
                         active_txns_.size() * sizeof(txn_id_t) + sizeof(uint32_t);
        for (const auto &entry : tombstones_) {
            total += sizeof(uint32_t) + entry.table_name.size() + sizeof(Rid);
        }
        if (total > UINT32_MAX) throw std::runtime_error("checkpoint log is too large");
        log_tot_len_ = static_cast<uint32_t>(total);
    }
    void serialize(char *dest) const override;
    void deserialize(const char *src) override;

    std::vector<txn_id_t> active_txns_;
    std::vector<CheckpointTombstone> tombstones_;
};

std::unique_ptr<LogRecord> deserialize_log_record(const char *data, size_t size);

class LogBuffer {
public:
    LogBuffer() { reset(); }
    void reset() {
        offset_ = 0;
        std::memset(buffer_, 0, sizeof(buffer_));
    }
    bool is_full(size_t append_size) const {
        return static_cast<size_t>(offset_) + append_size > LOG_BUFFER_SIZE;
    }

    char buffer_[LOG_BUFFER_SIZE];
    int offset_{0};
};

class DiskManager;

class LogManager {
public:
    explicit LogManager(DiskManager *disk_manager) : disk_manager_(disk_manager) {}

    lsn_t add_log_to_buffer(LogRecord *log_record);
    void flush_log_to_disk();
    void force_log_to_disk();
    void initialize_from_disk(lsn_t next_lsn, int64_t valid_log_size);

    int64_t current_log_offset();
    lsn_t get_persist_lsn() const { return persist_lsn_.load(); }
    lsn_t get_next_lsn() const { return global_lsn_.load(); }
    LogBuffer *get_log_buffer() { return &log_buffer_; }

private:
    void flush_log_to_disk_unlocked(bool sync);
    void initialize_size_unlocked();

    std::atomic<lsn_t> global_lsn_{0};
    std::atomic<lsn_t> persist_lsn_{INVALID_LSN};
    std::mutex latch_;
    LogBuffer log_buffer_;
    DiskManager *disk_manager_;
    int64_t persisted_bytes_{-1};
    lsn_t buffer_last_lsn_{INVALID_LSN};
};
