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
#include "common/config.h"
#include "common/index_runtime.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kMaxLogRecordSize = static_cast<size_t>(LOG_BUFFER_SIZE);

template <typename T>
bool ReadScalar(const char *data, size_t size, size_t offset, T *value) {
    if (value == nullptr || offset > size || sizeof(T) > size - offset) {
        return false;
    }
    memcpy(value, data + offset, sizeof(T));
    return true;
}

bool ConsumeBytes(size_t amount, size_t size, size_t *offset) {
    if (offset == nullptr || *offset > size || amount > size - *offset) {
        return false;
    }
    *offset += amount;
    return true;
}

bool ConsumeRecordImage(const char *data, size_t size, size_t *offset) {
    int record_size = 0;
    if (!ReadScalar(data, size, *offset, &record_size) ||
        !ConsumeBytes(sizeof(int), size, offset) || record_size <= 0 || record_size > RM_MAX_RECORD_SIZE) {
        return false;
    }
    return ConsumeBytes(static_cast<size_t>(record_size), size, offset);
}

bool ConsumeRidAndTableName(const char *data, size_t size, size_t *offset) {
    if (!ConsumeBytes(sizeof(Rid), size, offset)) {
        return false;
    }
    size_t table_name_size = 0;
    if (!ReadScalar(data, size, *offset, &table_name_size) ||
        !ConsumeBytes(sizeof(size_t), size, offset) || table_name_size == 0) {
        return false;
    }
    return ConsumeBytes(table_name_size, size, offset) && *offset == size;
}

bool ValidateLogRecord(const char *data, size_t size, lsn_t expected_lsn) {
    if (data == nullptr || size < static_cast<size_t>(LOG_HEADER_SIZE) || size > kMaxLogRecordSize ||
        expected_lsn < 0) {
        return false;
    }

    LogType log_type;
    lsn_t stored_lsn = INVALID_LSN;
    uint32_t total_len = 0;
    lsn_t prev_lsn = INVALID_LSN;
    if (!ReadScalar(data, size, OFFSET_LOG_TYPE, &log_type) ||
        !ReadScalar(data, size, OFFSET_LSN, &stored_lsn) ||
        !ReadScalar(data, size, OFFSET_LOG_TOT_LEN, &total_len) ||
        !ReadScalar(data, size, OFFSET_PREV_LSN, &prev_lsn) || total_len != size || stored_lsn != expected_lsn ||
        (prev_lsn != INVALID_LSN && (prev_lsn < 0 || prev_lsn >= stored_lsn))) {
        return false;
    }

    size_t offset = OFFSET_LOG_DATA;
    switch (log_type) {
        case LogType::begin:
        case LogType::commit:
        case LogType::ABORT:
            return size == static_cast<size_t>(LOG_HEADER_SIZE);
        case LogType::CHECKPOINT: {
            int active_txn_count = 0;
            if (!ReadScalar(data, size, offset, &active_txn_count) ||
                !ConsumeBytes(sizeof(int), size, &offset) || active_txn_count < 0) {
                return false;
            }
            constexpr size_t entry_size = sizeof(txn_id_t) + sizeof(lsn_t);
            size_t remaining = size - offset;
            if (static_cast<size_t>(active_txn_count) > remaining / entry_size ||
                static_cast<size_t>(active_txn_count) * entry_size != remaining) {
                return false;
            }
            for (int i = 0; i < active_txn_count; ++i) {
                txn_id_t txn_id = INVALID_TXN_ID;
                lsn_t last_lsn = INVALID_LSN;
                if (!ReadScalar(data, size, offset, &txn_id) ||
                    !ConsumeBytes(sizeof(txn_id_t), size, &offset) ||
                    !ReadScalar(data, size, offset, &last_lsn) ||
                    !ConsumeBytes(sizeof(lsn_t), size, &offset) || txn_id == INVALID_TXN_ID ||
                    (last_lsn != INVALID_LSN && (last_lsn < 0 || last_lsn >= stored_lsn))) {
                    return false;
                }
            }
            return offset == size;
        }
        case LogType::INSERT:
        case LogType::DELETE:
            return ConsumeRecordImage(data, size, &offset) && ConsumeRidAndTableName(data, size, &offset);
        case LogType::UPDATE:
            return ConsumeRecordImage(data, size, &offset) && ConsumeRecordImage(data, size, &offset) &&
                   ConsumeRidAndTableName(data, size, &offset);
        default:
            return false;
    }
}

// Parse a single log record from raw bytes at src; deserialized fields go into the provided record.
// Returns nullptr if the type is unknown.
static std::unique_ptr<LogRecord> ParseLogRecord(const char* src) {
    LogType log_type;
    memcpy(&log_type, src + OFFSET_LOG_TYPE, sizeof(LogType));
    std::unique_ptr<LogRecord> rec;
    switch (log_type) {
        case LogType::begin:
            rec = std::make_unique<BeginLogRecord>();
            break;
        case LogType::commit:
            rec = std::make_unique<CommitLogRecord>();
            break;
        case LogType::ABORT:
            rec = std::make_unique<AbortLogRecord>();
            break;
        case LogType::INSERT:
            rec = std::make_unique<InsertLogRecord>();
            break;
        case LogType::DELETE:
            rec = std::make_unique<DeleteLogRecord>();
            break;
        case LogType::UPDATE:
            rec = std::make_unique<UpdateLogRecord>();
            break;
        case LogType::CHECKPOINT:
            rec = std::make_unique<CheckpointLogRecord>();
            break;
        default:
            return nullptr;
    }
    rec->deserialize(src);
    return rec;
}

enum class StreamReadStatus { kRecord, kEnd, kInvalid };

class LogRecordStream {
public:
    LogRecordStream(DiskManager *disk_manager, lsn_t start_lsn, lsn_t end_lsn)
        : disk_manager_(disk_manager), buffer_(kMaxLogRecordSize + kReadChunkSize),
          buffer_start_lsn_(start_lsn), end_lsn_(end_lsn) {}

    StreamReadStatus Next(std::unique_ptr<LogRecord> *record) {
        if (record == nullptr || disk_manager_ == nullptr || buffer_start_lsn_ < 0 || end_lsn_ < buffer_start_lsn_) {
            return StreamReadStatus::kInvalid;
        }
        record->reset();
        lsn_t record_lsn = next_lsn();
        if (record_lsn >= end_lsn_) {
            return StreamReadStatus::kEnd;
        }
        if (!EnsureAvailable(LOG_HEADER_SIZE)) {
            return StreamReadStatus::kInvalid;
        }

        record_lsn = next_lsn();
        uint32_t total_len = 0;
        memcpy(&total_len, buffer_.data() + position_ + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        if (total_len < static_cast<uint32_t>(LOG_HEADER_SIZE) || total_len > kMaxLogRecordSize ||
            static_cast<lsn_t>(total_len) > end_lsn_ - record_lsn || !EnsureAvailable(total_len)) {
            return StreamReadStatus::kInvalid;
        }

        record_lsn = next_lsn();
        const char *record_data = buffer_.data() + position_;
        if (!ValidateLogRecord(record_data, total_len, record_lsn)) {
            return StreamReadStatus::kInvalid;
        }
        *record = ParseLogRecord(record_data);
        if (*record == nullptr) {
            return StreamReadStatus::kInvalid;
        }
        position_ += total_len;
        return StreamReadStatus::kRecord;
    }

    lsn_t next_lsn() const { return buffer_start_lsn_ + static_cast<lsn_t>(position_); }

private:
    bool EnsureAvailable(size_t needed) {
        if (valid_bytes_ - position_ >= needed) {
            return true;
        }
        if (position_ > 0) {
            size_t remaining = valid_bytes_ - position_;
            memmove(buffer_.data(), buffer_.data() + position_, remaining);
            buffer_start_lsn_ += static_cast<lsn_t>(position_);
            valid_bytes_ = remaining;
            position_ = 0;
        }
        while (valid_bytes_ < needed) {
            lsn_t read_lsn = buffer_start_lsn_ + static_cast<lsn_t>(valid_bytes_);
            if (read_lsn >= end_lsn_) {
                return false;
            }
            size_t room = buffer_.size() - valid_bytes_;
            size_t file_remaining = static_cast<size_t>(end_lsn_ - read_lsn);
            size_t read_size = std::min(kReadChunkSize, std::min(room, file_remaining));
            if (read_size == 0) {
                return false;
            }
            size_t bytes_read = disk_manager_->read_log(buffer_.data() + valid_bytes_, read_size, read_lsn);
            if (bytes_read == 0) {
                return false;
            }
            valid_bytes_ += bytes_read;
        }
        return true;
    }

    static constexpr size_t kReadChunkSize = 1024 * 1024;
    DiskManager *disk_manager_;
    std::vector<char> buffer_;
    lsn_t buffer_start_lsn_;
    lsn_t end_lsn_;
    size_t position_{0};
    size_t valid_bytes_{0};
};

static std::unique_ptr<LogRecord> ReadLogRecordAt(DiskManager *disk_manager, lsn_t lsn, int64_t file_size) {
    if (disk_manager == nullptr || lsn == INVALID_LSN || lsn < 0 || file_size < 0 || lsn > file_size ||
        file_size - lsn < LOG_HEADER_SIZE) {
        return nullptr;
    }

    char header[LOG_HEADER_SIZE];
    size_t header_bytes = disk_manager->read_log(header, LOG_HEADER_SIZE, lsn);
    if (header_bytes < LOG_HEADER_SIZE) {
        return nullptr;
    }

    uint32_t total_len = 0;
    memcpy(&total_len, header + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
    if (total_len < static_cast<uint32_t>(LOG_HEADER_SIZE) || total_len > kMaxLogRecordSize ||
        static_cast<int64_t>(total_len) > file_size - lsn) {
        return nullptr;
    }

    std::vector<char> buffer(total_len);
    memcpy(buffer.data(), header, LOG_HEADER_SIZE);
    if (total_len > LOG_HEADER_SIZE) {
        size_t body_bytes = disk_manager->read_log(buffer.data() + LOG_HEADER_SIZE, total_len - LOG_HEADER_SIZE,
                                                   lsn + LOG_HEADER_SIZE);
        if (body_bytes < static_cast<size_t>(total_len - LOG_HEADER_SIZE)) {
            return nullptr;
        }
    }
    if (!ValidateLogRecord(buffer.data(), buffer.size(), lsn)) {
        return nullptr;
    }
    return ParseLogRecord(buffer.data());
}

}  // namespace

// Helpers for index key extraction
static void BuildIndexKey(const std::vector<ColMeta>& cols, const char* data, std::unique_ptr<char[]>& key) {
    int offset = 0;
    for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
        memcpy(key.get() + offset, data + cols[i].offset, cols[i].len);
        offset += cols[i].len;
    }
}

static void EnsureRidPageExists(RmFileHandle *fh, BufferPoolManager *buffer_pool_manager, const Rid &rid) {
    while (rid.page_no >= fh->get_file_hdr().num_pages) {
        auto page_handle = fh->create_new_page_handle();
        buffer_pool_manager->unpin_page(page_handle.page->get_page_id(), true);
    }
}

/**
 * @description: analyze阶段，扫描日志，识别已提交和未完成的事务。
 * 如果存在检查点重启文件，则从检查点开始扫描。
 */
void RecoveryManager::analyze() {
    std::unordered_set<txn_id_t>().swap(committed_txns_);
    std::unordered_map<txn_id_t, lsn_t>().swap(uncommitted_last_lsn_);
    scan_start_lsn_ = 0;
    scan_end_lsn_ = 0;

    int64_t file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;

    lsn_t start_offset = 0;
    std::unordered_set<txn_id_t> active;
    std::unordered_map<txn_id_t, lsn_t> active_last_lsn;

    // Check for checkpoint restart file
    std::ifstream chkpt_file(CHECKPOINT_FILE_NAME);
    if (chkpt_file.is_open()) {
        lsn_t checkpoint_offset = INVALID_LSN;
        chkpt_file >> checkpoint_offset;
        chkpt_file.close();
        if (checkpoint_offset >= 0 && checkpoint_offset < file_size) {
            auto ckpt_rec = ReadLogRecordAt(disk_manager_, checkpoint_offset, file_size);
            if (ckpt_rec != nullptr && ckpt_rec->log_type_ == LogType::CHECKPOINT) {
                auto* ckpt = dynamic_cast<CheckpointLogRecord*>(ckpt_rec.get());
                for (int i = 0; i < ckpt->active_txn_count_; ++i) {
                    active.insert(ckpt->active_txns_[i].txn_id_);
                    active_last_lsn[ckpt->active_txns_[i].txn_id_] = ckpt->active_txns_[i].last_lsn_;
                }
                start_offset = checkpoint_offset + ckpt->log_tot_len_;
            }
        }
    }

    scan_start_lsn_ = start_offset;
    scan_end_lsn_ = start_offset;
    LogRecordStream stream(disk_manager_, start_offset, file_size);
    while (true) {
        std::unique_ptr<LogRecord> rec;
        StreamReadStatus status = stream.Next(&rec);
        if (status == StreamReadStatus::kEnd) {
            break;
        }
        if (status == StreamReadStatus::kInvalid) {
            std::cerr << "Ignoring invalid or incomplete WAL at LSN " << stream.next_lsn() << "\n";
            break;
        }
        scan_end_lsn_ = stream.next_lsn();
        txn_id_t tid = rec->log_tid_;
        if (tid != INVALID_TXN_ID) {
            active_last_lsn[tid] = rec->lsn_;
        }
        if (rec->log_type_ == LogType::begin) {
            active.insert(tid);
        } else if (rec->log_type_ == LogType::commit) {
            active.erase(tid);
            committed_txns_.insert(tid);
            active_last_lsn.erase(tid);
        } else if (rec->log_type_ == LogType::ABORT) {
            active.erase(tid);
            active_last_lsn.erase(tid);
        }
    }
    for (txn_id_t tid : active) {
        auto iter = active_last_lsn.find(tid);
        uncommitted_last_lsn_[tid] = iter == active_last_lsn.end() ? INVALID_LSN : iter->second;
    }
}

/**
 * @description: 重做已提交事务的所有操作。
 * 由于 MVCC DELETE 不修改数据页，需要在 REDO 阶段真正删除已提交删除的记录。
 */
void RecoveryManager::redo() {
    LogRecordStream stream(disk_manager_, scan_start_lsn_, scan_end_lsn_);
    while (true) {
        std::unique_ptr<LogRecord> rec;
        StreamReadStatus status = stream.Next(&rec);
        if (status == StreamReadStatus::kEnd) {
            break;
        }
        if (status == StreamReadStatus::kInvalid) {
            std::cerr << "Stopping REDO at invalid WAL LSN " << stream.next_lsn() << "\n";
            break;
        }
        txn_id_t tid = rec->log_tid_;
        if (committed_txns_.find(tid) == committed_txns_.end()) continue;

        std::string tab_name;
        Rid rid;
        RmFileHandle* fh = nullptr;
        TabMeta tab;

        switch (rec->log_type_) {
            case LogType::INSERT: {
                auto* insert_rec = dynamic_cast<InsertLogRecord*>(rec.get());
                tab_name = insert_rec->table_name_;
                rid = insert_rec->rid_;
                if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) break;
                fh = sm_manager_->fhs_.at(tab_name).get();
                tab = sm_manager_->db_.get_table(tab_name);
                auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);

                bool record_exists = false;
                try {
                    auto existing = fh->get_record(rid, nullptr);
                    record_exists = existing != nullptr;
                } catch (...) {
                    record_exists = false;
                }

                if (!record_exists) {
                    EnsureRidPageExists(fh, buffer_pool_manager_, rid);
                    try {
                        fh->insert_record(rid, insert_rec->insert_value_.data);
                    } catch (...) {
                    }
                }

                for (const auto &binding : bindings) {
                    auto key = rmdb::build_index_key(*binding.meta, insert_rec->insert_value_.data, rid);
                    auto outcome = binding.ih->insert_entry(key.get(), rid, nullptr);
                    if (outcome.result == IxInsertResult::kDuplicate) {
                    }
                }
                break;
            }
            case LogType::DELETE: {
                // MVCC DELETE 不修改数据页，REDO 阶段需要真正删除记录和索引
                auto* delete_rec = dynamic_cast<DeleteLogRecord*>(rec.get());
                tab_name = delete_rec->table_name_;
                rid = delete_rec->rid_;
                if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) break;
                fh = sm_manager_->fhs_.at(tab_name).get();
                tab = sm_manager_->db_.get_table(tab_name);
                auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
                // Delete index entries (they were rebuilt by open_db)
                for (const auto &binding : bindings) {
                    auto key = rmdb::build_index_key(*binding.meta, delete_rec->old_record_.data, rid);
                    binding.ih->delete_entry(key.get(), nullptr);
                }
                // Actually free the slot on the data page
                try { fh->delete_record(rid, nullptr); } catch (...) {}
                break;
            }
            case LogType::UPDATE: {
                // 已提交的 UPDATE，确保数据页和索引反映最新状态
                // 需要先删除新旧索引条目以使操作幂等（open_db可能已重建索引）
                auto* update_rec = dynamic_cast<UpdateLogRecord*>(rec.get());
                tab_name = update_rec->table_name_;
                rid = update_rec->rid_;
                if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) break;
                fh = sm_manager_->fhs_.at(tab_name).get();
                tab = sm_manager_->db_.get_table(tab_name);
                auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
                // Delete both old and new index entries (new might exist from open_db rebuild)
                for (const auto &binding : bindings) {
                    auto old_key = rmdb::build_index_key(*binding.meta, update_rec->old_record_.data, rid);
                    try { binding.ih->delete_entry(old_key.get(), nullptr); } catch (...) {}
                    auto new_key = rmdb::build_index_key(*binding.meta, update_rec->new_record_.data, rid);
                    try { binding.ih->delete_entry(new_key.get(), nullptr); } catch (...) {}
                }
                fh->update_record(rid, update_rec->new_record_.data, nullptr);
                for (const auto &binding : bindings) {
                    auto new_key = rmdb::build_index_key(*binding.meta, update_rec->new_record_.data, rid);
                    auto outcome = binding.ih->insert_entry(new_key.get(), rid, nullptr);
                    if (outcome.result == IxInsertResult::kDuplicate) {
                    }
                }
                break;
            }
            default: break;
        }
    }
    std::unordered_set<txn_id_t>().swap(committed_txns_);
}

/**
 * @description: 回滚未完成的事务。
 * 由于 open_db() 会根据数据页重建索引，UNDO 主要处理：
 * - INSERT: 从数据页删除记录，并清理索引
 * - DELETE: MVCC DELETE 不修改数据页，崩溃后 TupleMeta 丢失，记录自动变为可见。无需额外操作。
 * - UPDATE: 恢复旧数据，更新索引
 */
void RecoveryManager::undo() {
    int64_t file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    auto undo_one = [&](const LogRecord *rec) {
        std::string tab_name;
        Rid rid;
        RmFileHandle* fh = nullptr;
        TabMeta tab;

        switch (rec->log_type_) {
            case LogType::INSERT: {
                // Undo INSERT: 删除插入的记录及其索引
                auto* insert_rec = dynamic_cast<const InsertLogRecord*>(rec);
                tab_name = insert_rec->table_name_;
                rid = insert_rec->rid_;
                if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) break;
                fh = sm_manager_->fhs_.at(tab_name).get();
                tab = sm_manager_->db_.get_table(tab_name);
                auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
                for (const auto &binding : bindings) {
                    auto key = rmdb::build_index_key(*binding.meta, insert_rec->insert_value_.data, rid);
                    binding.ih->delete_entry(key.get(), nullptr);
                }
                try { fh->delete_record(rid, nullptr); } catch (...) {}
                break;
            }
            case LogType::DELETE: {
                // Undo DELETE: MVCC DELETE 不修改数据页，崩溃后 TupleMeta 丢失，
                // 记录自动恢复为可见，且 open_db 已重建索引。无需额外操作。
                break;
            }
            case LogType::UPDATE: {
                // Undo UPDATE: 恢复旧记录数据和旧索引（先删除新旧索引条目以使操作幂等）
                auto* update_rec = dynamic_cast<const UpdateLogRecord*>(rec);
                tab_name = update_rec->table_name_;
                rid = update_rec->rid_;
                if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) break;
                fh = sm_manager_->fhs_.at(tab_name).get();
                tab = sm_manager_->db_.get_table(tab_name);
                auto bindings = rmdb::bind_table_indexes(sm_manager_, tab_name, tab);
                for (const auto &binding : bindings) {
                    auto new_key = rmdb::build_index_key(*binding.meta, update_rec->new_record_.data, rid);
                    try { binding.ih->delete_entry(new_key.get(), nullptr); } catch (...) {}
                    auto old_key = rmdb::build_index_key(*binding.meta, update_rec->old_record_.data, rid);
                    try { binding.ih->delete_entry(old_key.get(), nullptr); } catch (...) {}
                }
                fh->update_record(rid, update_rec->old_record_.data, nullptr);
                for (const auto &binding : bindings) {
                    auto old_key = rmdb::build_index_key(*binding.meta, update_rec->old_record_.data, rid);
                    auto outcome = binding.ih->insert_entry(old_key.get(), rid, nullptr);
                    if (outcome.result == IxInsertResult::kDuplicate) {
                    }
                }
                break;
            }
            default: break;
        }
    };

    for (const auto &entry : uncommitted_last_lsn_) {
        txn_id_t tid = entry.first;
        lsn_t current_lsn = entry.second;
        while (current_lsn != INVALID_LSN) {
            auto rec = ReadLogRecordAt(disk_manager_, current_lsn, file_size);
            if (rec == nullptr) {
                break;
            }
            if (rec->log_tid_ != tid) {
                current_lsn = rec->prev_lsn_;
                continue;
            }
            undo_one(rec.get());
            current_lsn = rec->prev_lsn_;
        }
    }

    std::unordered_map<txn_id_t, lsn_t>().swap(uncommitted_last_lsn_);
    scan_start_lsn_ = 0;
    scan_end_lsn_ = 0;
}
