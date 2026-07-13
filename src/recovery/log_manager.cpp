/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_manager.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "storage/disk_manager.h"

namespace {

template <typename T>
void put(char *dest, int &offset, const T &value) {
    std::memcpy(dest + offset, &value, sizeof(T));
    offset += sizeof(T);
}

template <typename T>
T get(const char *src, int &offset) {
    T value{};
    std::memcpy(&value, src + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

void put_record(char *dest, int &offset, const RmRecord &record) {
    const int32_t size = record.size;
    put(dest, offset, size);
    if (size > 0) {
        std::memcpy(dest + offset, record.data, size);
        offset += size;
    }
}

void get_record(const char *src, int &offset, RmRecord &record) {
    const int32_t size = get<int32_t>(src, offset);
    if (size < 0) throw std::runtime_error("negative record length in log");
    record = RmRecord(size);
    if (size > 0) {
        std::memcpy(record.data, src + offset, size);
        offset += size;
    }
}

void put_string(char *dest, int &offset, const std::string &value) {
    if (value.size() > UINT32_MAX) throw std::runtime_error("log string is too large");
    const uint32_t size = static_cast<uint32_t>(value.size());
    put(dest, offset, size);
    if (size > 0) {
        std::memcpy(dest + offset, value.data(), size);
        offset += static_cast<int>(size);
    }
}

void get_string(const char *src, int &offset, std::string &value) {
    const uint32_t size = get<uint32_t>(src, offset);
    value.assign(src + offset, src + offset + size);
    offset += static_cast<int>(size);
}

}  // namespace

void InsertLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    put_record(dest, offset, insert_value_);
    put(dest, offset, rid_);
    put_string(dest, offset, table_name_);
}

void InsertLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    get_record(src, offset, insert_value_);
    rid_ = get<Rid>(src, offset);
    get_string(src, offset, table_name_);
}

void DeleteLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    put_record(dest, offset, delete_value_);
    put(dest, offset, rid_);
    const uint8_t logical = logical_delete_ ? 1 : 0;
    put(dest, offset, logical);
    put_string(dest, offset, table_name_);
}

void DeleteLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    get_record(src, offset, delete_value_);
    rid_ = get<Rid>(src, offset);
    logical_delete_ = get<uint8_t>(src, offset) != 0;
    get_string(src, offset, table_name_);
}

void UpdateLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    put_record(dest, offset, old_value_);
    put_record(dest, offset, new_value_);
    put(dest, offset, rid_);
    const uint8_t revival = tombstone_revival_ ? 1 : 0;
    put(dest, offset, revival);
    put_string(dest, offset, table_name_);
}

void UpdateLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    get_record(src, offset, old_value_);
    get_record(src, offset, new_value_);
    rid_ = get<Rid>(src, offset);
    tombstone_revival_ = get<uint8_t>(src, offset) != 0;
    get_string(src, offset, table_name_);
}

void CheckpointLogRecord::serialize(char *dest) const {
    LogRecord::serialize(dest);
    int offset = OFFSET_LOG_DATA;
    const uint32_t active_count = static_cast<uint32_t>(active_txns_.size());
    put(dest, offset, active_count);
    for (txn_id_t txn_id : active_txns_) put(dest, offset, txn_id);
    const uint32_t tombstone_count = static_cast<uint32_t>(tombstones_.size());
    put(dest, offset, tombstone_count);
    for (const auto &entry : tombstones_) {
        put_string(dest, offset, entry.table_name);
        put(dest, offset, entry.rid);
    }
}

void CheckpointLogRecord::deserialize(const char *src) {
    LogRecord::deserialize(src);
    int offset = OFFSET_LOG_DATA;
    const uint32_t active_count = get<uint32_t>(src, offset);
    active_txns_.clear();
    active_txns_.reserve(active_count);
    for (uint32_t i = 0; i < active_count; ++i) active_txns_.push_back(get<txn_id_t>(src, offset));
    const uint32_t tombstone_count = get<uint32_t>(src, offset);
    tombstones_.clear();
    tombstones_.reserve(tombstone_count);
    for (uint32_t i = 0; i < tombstone_count; ++i) {
        CheckpointTombstone entry;
        get_string(src, offset, entry.table_name);
        entry.rid = get<Rid>(src, offset);
        tombstones_.push_back(std::move(entry));
    }
}

std::unique_ptr<LogRecord> deserialize_log_record(const char *data, size_t size) {
    if (size < LOG_HEADER_SIZE) return nullptr;
    LogType type{};
    uint32_t total = 0;
    std::memcpy(&type, data + OFFSET_LOG_TYPE, sizeof(type));
    std::memcpy(&total, data + OFFSET_LOG_TOT_LEN, sizeof(total));
    if (total < LOG_HEADER_SIZE || total > size) return nullptr;

    std::unique_ptr<LogRecord> record;
    switch (type) {
        case LogType::UPDATE: record = std::make_unique<UpdateLogRecord>(); break;
        case LogType::INSERT: record = std::make_unique<InsertLogRecord>(); break;
        case LogType::DELETE: record = std::make_unique<DeleteLogRecord>(); break;
        case LogType::begin: record = std::make_unique<BeginLogRecord>(); break;
        case LogType::commit: record = std::make_unique<CommitLogRecord>(); break;
        case LogType::ABORT: record = std::make_unique<AbortLogRecord>(); break;
        case LogType::CHECKPOINT: record = std::make_unique<CheckpointLogRecord>(); break;
        default: return nullptr;
    }
    try {
        record->deserialize(data);
    } catch (...) {
        return nullptr;
    }
    return record;
}

void LogManager::initialize_size_unlocked() {
    if (persisted_bytes_ >= 0) return;
    const int size = disk_manager_->get_file_size(LOG_FILE_NAME);
    persisted_bytes_ = std::max(size, 0);
}

lsn_t LogManager::add_log_to_buffer(LogRecord *log_record) {
    if (log_record == nullptr) return INVALID_LSN;
    std::lock_guard<std::mutex> guard(latch_);
    initialize_size_unlocked();

    log_record->lsn_ = global_lsn_.fetch_add(1);
    const size_t size = log_record->log_tot_len_;
    if (size < LOG_HEADER_SIZE) throw std::runtime_error("invalid log record size");

    std::vector<char> serialized(size);
    log_record->serialize(serialized.data());

    if (size > LOG_BUFFER_SIZE) {
        flush_log_to_disk_unlocked(false);
        disk_manager_->write_log(serialized.data(), static_cast<int>(size));
        persisted_bytes_ += static_cast<int64_t>(size);
        persist_lsn_ = log_record->lsn_;
        return log_record->lsn_;
    }
    if (log_buffer_.is_full(size)) flush_log_to_disk_unlocked(false);
    std::memcpy(log_buffer_.buffer_ + log_buffer_.offset_, serialized.data(), size);
    log_buffer_.offset_ += static_cast<int>(size);
    buffer_last_lsn_ = log_record->lsn_;
    return log_record->lsn_;
}

void LogManager::flush_log_to_disk_unlocked(bool sync) {
    initialize_size_unlocked();
    if (log_buffer_.offset_ == 0) {
        if (sync) disk_manager_->sync_log();
        return;
    }
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    if (sync) disk_manager_->sync_log();
    persisted_bytes_ += log_buffer_.offset_;
    persist_lsn_ = buffer_last_lsn_;
    log_buffer_.reset();
    buffer_last_lsn_ = INVALID_LSN;
}

void LogManager::flush_log_to_disk() {
    std::lock_guard<std::mutex> guard(latch_);
    flush_log_to_disk_unlocked(false);
}

void LogManager::force_log_to_disk() {
    std::lock_guard<std::mutex> guard(latch_);
    flush_log_to_disk_unlocked(true);
}

int64_t LogManager::current_log_offset() {
    std::lock_guard<std::mutex> guard(latch_);
    initialize_size_unlocked();
    return persisted_bytes_ + log_buffer_.offset_;
}

void LogManager::initialize_from_disk(lsn_t next_lsn, int64_t valid_log_size) {
    std::lock_guard<std::mutex> guard(latch_);
    global_lsn_ = std::max<lsn_t>(next_lsn, 0);
    persisted_bytes_ = std::max<int64_t>(valid_log_size, 0);
    log_buffer_.reset();
    buffer_last_lsn_ = INVALID_LSN;
}
