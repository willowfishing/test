/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <chrono>
#include <cstring>
#include "log_manager.h"

namespace {
constexpr int kGroupCommitMaxWaitUs = 50;
}

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号（即该记录在日志文件中的字节偏移量）
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    lsn_t lsn = global_lsn_++;
    log_record->lsn_ = lsn;

    if (active_buffer_->is_full(log_record->log_tot_len_)) {
        flush_active_buffer_unlocked(lock);
    }

    // Track the file offset where this record will be written.
    log_record->lsn_ = active_start_offset_ + active_buffer_->offset_;
    log_record->serialize(active_buffer_->buffer_ + active_buffer_->offset_);
    active_buffer_->offset_ += log_record->log_tot_len_;
    next_log_offset_ = active_start_offset_ + active_buffer_->offset_;
    ++append_epoch_;
    group_flush_cv_.notify_one();
    return log_record->lsn_;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::unique_lock<std::mutex> lock(latch_);
    flush_log_to_disk_until_unlocked(lock, next_log_offset_ - 1);
}

void LogManager::flush_log_to_disk_until(lsn_t target_lsn) {
    std::unique_lock<std::mutex> lock(latch_);
    flush_log_to_disk_until_unlocked(lock, target_lsn);
}

void LogManager::flush_log_to_disk_until_group(lsn_t target_lsn) {
    std::unique_lock<std::mutex> lock(latch_);
    if (target_lsn == INVALID_LSN || target_lsn <= persist_lsn_) {
        return;
    }

    while (target_lsn > persist_lsn_) {
        if (flush_in_progress_ || group_leader_waiting_) {
            flush_cv_.wait(lock, [&] {
                return target_lsn <= persist_lsn_ || (!flush_in_progress_ && !group_leader_waiting_);
            });
            continue;
        }

        if (active_buffer_->offset_ == 0) {
            return;
        }

        group_leader_waiting_ = true;
        uint64_t observed_epoch = append_epoch_;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(kGroupCommitMaxWaitUs);
        while (target_lsn > persist_lsn_ && std::chrono::steady_clock::now() < deadline) {
            if (group_flush_cv_.wait_until(lock, deadline, [&] {
                    return append_epoch_ != observed_epoch || flush_in_progress_;
                })) {
                observed_epoch = append_epoch_;
                if (flush_in_progress_) {
                    break;
                }
            }
        }
        group_leader_waiting_ = false;
        flush_cv_.notify_all();

        if (target_lsn <= persist_lsn_) {
            return;
        }
        if (flush_in_progress_) {
            continue;
        }
        flush_active_buffer_unlocked(lock);
    }
}

void LogManager::flush_log_to_disk_until_unlocked(std::unique_lock<std::mutex> &lock, lsn_t target_lsn) {
    if (target_lsn == INVALID_LSN || target_lsn <= persist_lsn_) {
        return;
    }

    while (target_lsn > persist_lsn_) {
        if (flush_in_progress_) {
            flush_cv_.wait(lock, [&] { return !flush_in_progress_ || target_lsn <= persist_lsn_; });
            continue;
        }

        if (active_buffer_->offset_ > 0) {
            flush_active_buffer_unlocked(lock);
        } else {
            return;
        }
    }
}

void LogManager::flush_active_buffer_unlocked(std::unique_lock<std::mutex> &lock) {
    if (active_buffer_->offset_ == 0) {
        return;
    }
    while (flush_in_progress_) {
        flush_cv_.wait(lock, [&] { return !flush_in_progress_; });
    }

    std::swap(active_buffer_, flush_buffer_);
    flush_start_offset_ = active_start_offset_;
    flush_size_ = flush_buffer_->offset_;
    active_start_offset_ = flush_start_offset_ + flush_size_;
    flush_in_progress_ = true;

    char *flush_data = flush_buffer_->buffer_;
    int flush_size = flush_size_;
    lsn_t flush_start = flush_start_offset_;
    lock.unlock();
    try {
        disk_manager_->write_log(flush_data, static_cast<size_t>(flush_size), flush_start);
        lock.lock();
    } catch (...) {
        lock.lock();
        flush_in_progress_ = false;
        flush_cv_.notify_all();
        throw;
    }

    persist_lsn_ = flush_start + flush_size - 1;
    flush_buffer_->reset();
    flush_size_ = 0;
    flush_in_progress_ = false;
    flush_cv_.notify_all();
}

lsn_t LogManager::get_log_file_offset() {
    std::lock_guard<std::mutex> lock(latch_);
    return next_log_offset_;
}

lsn_t LogManager::get_persist_lsn() {
    std::lock_guard<std::mutex> lock(latch_);
    return persist_lsn_;
}

void LogManager::reset_log_file_offset(lsn_t log_file_offset) {
    std::unique_lock<std::mutex> lock(latch_);
    while (flush_in_progress_) {
        flush_cv_.wait(lock);
    }
    active_buffer_->reset();
    flush_buffer_->reset();
    active_start_offset_ = log_file_offset;
    next_log_offset_ = log_file_offset;
    persist_lsn_ = log_file_offset > 0 ? log_file_offset - 1 : INVALID_LSN;
    global_lsn_.store(0, std::memory_order_relaxed);
}
