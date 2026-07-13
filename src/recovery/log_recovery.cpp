/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "execution/index_key_utils.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "transaction/transaction_manager.h"

namespace {

constexpr const char *RESTART_FILE_NAME = "db.restart";
constexpr const char *RESTART_TEMP_FILE_NAME = "db.restart.tmp";
constexpr uint64_t RESTART_MAGIC = 0x524d4442434b5054ULL;  // "RMDBCKPT"
constexpr uint32_t MAX_REASONABLE_LOG_RECORD = 256U * 1024U * 1024U;

struct RestartRecord {
    uint64_t magic{RESTART_MAGIC};
    int64_t checkpoint_offset{0};
};

bool same_record(const RmRecord &lhs, const RmRecord &rhs) {
    return lhs.size == rhs.size &&
           (lhs.size == 0 || std::memcmp(lhs.data, rhs.data, lhs.size) == 0);
}

bool current_record_equals(RmFileHandle *fh, const Rid &rid,
                           const RmRecord &expected) {
    if (!fh->is_record(rid)) return false;
    auto current = fh->get_record(rid, nullptr);
    return current != nullptr && same_record(*current, expected);
}

void put_record_image(RmFileHandle *fh, const Rid &rid,
                      const RmRecord &record) {
    fh->ensure_page_exists(rid.page_no);
    if (fh->is_record(rid)) {
        if (!current_record_equals(fh, rid, record)) {
            fh->update_record(rid, record.data, nullptr);
        }
    } else {
        fh->insert_record(rid, record.data);
    }
}

bool same_rid(const Rid &lhs, const Rid &rhs) {
    return lhs.page_no == rhs.page_no && lhs.slot_no == rhs.slot_no;
}

void set_index_entry(SmManager *sm_manager, const std::string &table_name,
                     const IndexMeta &index, const RmRecord &record,
                     const Rid &rid, bool present) {
    const std::string index_name =
        sm_manager->get_ix_manager()->get_index_name(table_name, index.cols);
    auto handle_it = sm_manager->ihs_.find(index_name);
    if (handle_it == sm_manager->ihs_.end()) return;

    auto key = make_index_key(index, record.data);
    std::vector<Rid> existing;
    const bool found = handle_it->second->get_value(key.data(), &existing, nullptr);
    if (!present) {
        // A later committed version may already own this key on disk. Only
        // remove the exact tuple affected by this log record.
        if (found && !existing.empty() && same_rid(existing.front(), rid)) {
            handle_it->second->delete_entry(key.data(), nullptr);
        }
        return;
    }

    if (found && !existing.empty() && same_rid(existing.front(), rid)) return;
    if (found) {
        // During restart a page written before the crash can contain a later
        // image than the log currently being replayed. Replaying in LSN order
        // must temporarily replace that image; a later log will restore it.
        handle_it->second->delete_entry(key.data(), nullptr);
    }
    if (handle_it->second->insert_entry(key.data(), rid, nullptr) == IX_NO_PAGE) {
        throw UniqueConstraintError(table_name);
    }
}

void set_record_indexes(SmManager *sm_manager, const std::string &table_name,
                        const RmRecord &record, const Rid &rid, bool present) {
    const TabMeta &table = sm_manager->db_.get_table(table_name);
    for (const auto &index : table.indexes) {
        set_index_entry(sm_manager, table_name, index, record, rid, present);
    }
}

void replace_record_indexes(SmManager *sm_manager,
                            const std::string &table_name,
                            const RmRecord &old_record,
                            const RmRecord &new_record, const Rid &rid) {
    const TabMeta &table = sm_manager->db_.get_table(table_name);
    for (const auto &index : table.indexes) {
        auto old_key = make_index_key(index, old_record.data);
        auto new_key = make_index_key(index, new_record.data);
        if (old_key == new_key) {
            set_index_entry(sm_manager, table_name, index, new_record, rid, true);
            continue;
        }
        set_index_entry(sm_manager, table_name, index, old_record, rid, false);
        set_index_entry(sm_manager, table_name, index, new_record, rid, true);
    }
}

}  // namespace

int64_t RecoveryManager::read_restart_offset() const {
    std::ifstream input(RESTART_FILE_NAME, std::ios::binary);
    if (!input.is_open()) return 0;
    RestartRecord record;
    input.read(reinterpret_cast<char *>(&record), sizeof(record));
    if (!input || record.magic != RESTART_MAGIC || record.checkpoint_offset < 0) return 0;
    const int log_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (log_size < 0 || record.checkpoint_offset >= log_size) return 0;
    return record.checkpoint_offset;
}

void RecoveryManager::write_restart_offset(int64_t offset) const {
    RestartRecord record;
    record.checkpoint_offset = offset;
    int fd = ::open(RESTART_TEMP_FILE_NAME, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) throw UnixError();
    const ssize_t written = ::write(fd, &record, sizeof(record));
    if (written != static_cast<ssize_t>(sizeof(record)) || ::fsync(fd) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw UnixError();
    }
    if (::close(fd) != 0) throw UnixError();
    if (::rename(RESTART_TEMP_FILE_NAME, RESTART_FILE_NAME) != 0) throw UnixError();
    int dir_fd = ::open(".", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
}

RecoveryManager::~RecoveryManager() { unmap_log(); }

void RecoveryManager::unmap_log() {
    if (mapped_log_ != nullptr) {
        ::munmap(const_cast<char *>(mapped_log_), mapped_log_size_);
        mapped_log_ = nullptr;
        mapped_log_size_ = 0;
    }
}

void RecoveryManager::map_log(int64_t size) {
    unmap_log();
    if (size <= 0) return;
    const int fd = disk_manager_->get_file_fd(LOG_FILE_NAME);
    void *mapping = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ,
                           MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) throw UnixError();
    mapped_log_ = static_cast<const char *>(mapping);
    mapped_log_size_ = static_cast<size_t>(size);
}

std::unique_ptr<LogRecord> RecoveryManager::read_record(const LogMeta &meta) const {
    if (mapped_log_ == nullptr || meta.offset < 0 ||
        static_cast<uint64_t>(meta.offset) + meta.length > mapped_log_size_) {
        return nullptr;
    }
    return deserialize_log_record(mapped_log_ + meta.offset, meta.length);
}

void RecoveryManager::analyze() {
    logs_.clear();
    active_.clear();
    max_lsn_ = INVALID_LSN;
    max_txn_id_ = INVALID_TXN_ID;
    affected_pages_.clear();

    const int64_t log_size = std::max<int64_t>(0, disk_manager_->get_file_size(LOG_FILE_NAME));
    scan_start_ = read_restart_offset();
    valid_log_size_ = scan_start_;
    map_log(log_size);

    std::unordered_map<txn_id_t, std::vector<size_t>> pending_logs;
    int64_t offset = scan_start_;
    while (mapped_log_ != nullptr && offset + LOG_HEADER_SIZE <= log_size) {
        const char *header = mapped_log_ + offset;
        LogType type{};
        uint32_t length = 0;
        lsn_t lsn = INVALID_LSN;
        txn_id_t txn_id = INVALID_TXN_ID;
        std::memcpy(&type, header + OFFSET_LOG_TYPE, sizeof(type));
        std::memcpy(&length, header + OFFSET_LOG_TOT_LEN, sizeof(length));
        std::memcpy(&lsn, header + OFFSET_LSN, sizeof(lsn));
        std::memcpy(&txn_id, header + OFFSET_LOG_TID, sizeof(txn_id));
        if (type < LogType::UPDATE || type > LogType::CHECKPOINT ||
            length < LOG_HEADER_SIZE || length > MAX_REASONABLE_LOG_RECORD ||
            offset + length > log_size) {
            break;
        }

        valid_log_size_ = offset + length;
        max_lsn_ = std::max(max_lsn_, lsn);
        if (txn_id != INVALID_TXN_ID) max_txn_id_ = std::max(max_txn_id_, txn_id);

        if (type == LogType::CHECKPOINT) {
            LogMeta meta{offset, length, type, lsn, txn_id};
            auto record = read_record(meta);
            auto *checkpoint = record == nullptr
                                   ? nullptr
                                   : dynamic_cast<CheckpointLogRecord *>(record.get());
            if (checkpoint == nullptr) break;
            active_.clear();
            active_.insert(checkpoint->active_txns_.begin(), checkpoint->active_txns_.end());
            txn_manager_->restore_tombstones(checkpoint->tombstones_);
        } else if (type == LogType::begin) {
            active_.insert(txn_id);
        } else if (type == LogType::INSERT || type == LogType::DELETE ||
                   type == LogType::UPDATE) {
            active_.insert(txn_id);
            logs_.push_back(LogMeta{offset, length, type, lsn, txn_id});
            pending_logs[txn_id].push_back(logs_.size() - 1);
        } else if (type == LogType::commit) {
            auto pending = pending_logs.find(txn_id);
            if (pending != pending_logs.end()) {
                for (size_t index : pending->second) logs_[index].committed = true;
                pending_logs.erase(pending);
            }
            active_.erase(txn_id);
        } else if (type == LogType::ABORT) {
            auto pending = pending_logs.find(txn_id);
            if (pending != pending_logs.end()) {
                for (size_t index : pending->second) logs_[index].aborted = true;
                pending_logs.erase(pending);
            }
            active_.erase(txn_id);
        }
        offset += length;
    }

    // Ignore a torn tail left by the crash. New WAL appends after the last
    // complete record. Remap after truncation so later passes never touch a
    // page beyond the new end of file.
    if (valid_log_size_ < log_size) {
        unmap_log();
        disk_manager_->truncate_log(valid_log_size_);
        map_log(valid_log_size_);
    }
    log_manager_->initialize_from_disk(max_lsn_ == INVALID_LSN ? 0 : max_lsn_ + 1,
                                       valid_log_size_);
    if (max_txn_id_ != INVALID_TXN_ID) txn_manager_->set_next_txn_id(max_txn_id_ + 1);
}

void RecoveryManager::apply_redo(const LogRecord &base) {
    if (base.log_type_ == LogType::INSERT) {
        const auto &record = static_cast<const InsertLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        put_record_image(table->second.get(), record.rid_, record.insert_value_);
        set_record_indexes(sm_manager_, record.table_name_, record.insert_value_,
                           record.rid_, true);
        txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
    } else if (base.log_type_ == LogType::DELETE) {
        const auto &record = static_cast<const DeleteLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        if (record.logical_delete_) {
            txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, true);
        } else if (table->second->is_record(record.rid_)) {
            set_record_indexes(sm_manager_, record.table_name_, record.delete_value_,
                               record.rid_, false);
            table->second->delete_record(record.rid_, nullptr);
            txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
        }
    } else if (base.log_type_ == LogType::UPDATE) {
        const auto &record = static_cast<const UpdateLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        put_record_image(table->second.get(), record.rid_, record.new_value_);
        replace_record_indexes(sm_manager_, record.table_name_, record.old_value_,
                               record.new_value_, record.rid_);
        txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
    }
}

void RecoveryManager::apply_undo(const LogRecord &base) {
    if (base.log_type_ == LogType::INSERT) {
        const auto &record = static_cast<const InsertLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        if (current_record_equals(table->second.get(), record.rid_, record.insert_value_)) {
            set_record_indexes(sm_manager_, record.table_name_, record.insert_value_,
                               record.rid_, false);
            table->second->delete_record(record.rid_, nullptr);
        }
        txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
    } else if (base.log_type_ == LogType::DELETE) {
        const auto &record = static_cast<const DeleteLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        if (record.logical_delete_) {
            set_record_indexes(sm_manager_, record.table_name_, record.delete_value_,
                               record.rid_, true);
            txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
        } else {
            put_record_image(table->second.get(), record.rid_, record.delete_value_);
            set_record_indexes(sm_manager_, record.table_name_, record.delete_value_,
                               record.rid_, true);
            txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_, false);
        }
    } else if (base.log_type_ == LogType::UPDATE) {
        const auto &record = static_cast<const UpdateLogRecord &>(base);
        auto table = sm_manager_->fhs_.find(record.table_name_);
        if (table == sm_manager_->fhs_.end()) return;
        affected_pages_[record.table_name_].insert(record.rid_.page_no);
        put_record_image(table->second.get(), record.rid_, record.old_value_);
        replace_record_indexes(sm_manager_, record.table_name_, record.new_value_,
                               record.old_value_, record.rid_);
        txn_manager_->set_recovered_tombstone(record.table_name_, record.rid_,
                                              record.tombstone_revival_);
    }
}

void RecoveryManager::redo() {
    // Without a checkpoint the WAL contains the complete committed history.
    // Recreate empty B+ trees and replay index changes in LSN order instead of
    // scanning every table and rebuilding every index after recovery.
    if (scan_start_ == 0 && !logs_.empty()) {
        sm_manager_->reset_all_indexes();
    }
    for (const auto &meta : logs_) {
        if (!meta.committed) continue;
        if (meta.type != LogType::INSERT && meta.type != LogType::DELETE &&
            meta.type != LogType::UPDATE) {
            continue;
        }
        auto record = read_record(meta);
        if (record != nullptr) apply_redo(*record);
    }
}

void RecoveryManager::finish_recovery() {
    for (auto &[table_name, pages_set] : affected_pages_) {
        auto handle = sm_manager_->fhs_.find(table_name);
        if (handle == sm_manager_->fhs_.end()) continue;
        std::vector<int> pages(pages_set.begin(), pages_set.end());
        handle->second->repair_free_page_list(pages);
    }

    // Do not synchronously rewrite the recovered database before accepting
    // clients. Recovered pages remain dirty in the buffer pool and are still
    // protected by the WAL; a second crash simply replays the same idempotent
    // records. The next eviction, shutdown, or checkpoint persists them.
    unmap_log();
}

void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        if (it->committed || it->aborted || active_.count(it->txn_id) == 0) continue;
        if (it->type != LogType::INSERT && it->type != LogType::DELETE &&
            it->type != LogType::UPDATE) {
            continue;
        }
        auto record = read_record(*it);
        if (record != nullptr) apply_undo(*record);
    }
    finish_recovery();
}

void RecoveryManager::create_static_checkpoint(txn_id_t checkpoint_txn) {
    // The caller holds the global exclusive checkpoint gate. Explicit
    // transactions that are idle between client requests are aborted so the
    // checkpoint contains no uncommitted physical state.
    txn_manager_->abort_all_active(log_manager_, checkpoint_txn);
    log_manager_->force_log_to_disk();

    const auto active = txn_manager_->active_transaction_ids(checkpoint_txn);
    const auto tombstones = txn_manager_->snapshot_tombstones();
    const int64_t checkpoint_offset = log_manager_->current_log_offset();
    CheckpointLogRecord checkpoint(active, tombstones);
    log_manager_->add_log_to_buffer(&checkpoint);
    log_manager_->force_log_to_disk();

    sm_manager_->flush_all_pages();
    disk_manager_->sync_log();
    // Publish the restart address only after both WAL and database pages are
    // durable. A crash before this rename safely falls back to the old point.
    write_restart_offset(checkpoint_offset);
}
