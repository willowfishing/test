#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>

class RuntimePerfStats {
   public:
    RuntimePerfStats(std::string path, size_t buffer_pool_pages)
        : path_(std::move(path)), buffer_pool_pages_(buffer_pool_pages) {}

    void RecordBufferPoolHit() { buffer_pool_hits_.fetch_add(1, std::memory_order_relaxed); }
    void RecordBufferPoolMiss() { buffer_pool_misses_.fetch_add(1, std::memory_order_relaxed); }
    void RecordBufferPoolEviction() { buffer_pool_evictions_.fetch_add(1, std::memory_order_relaxed); }
    void RecordDirtyFlush() { dirty_flushes_.fetch_add(1, std::memory_order_relaxed); }
    void RecordPinFailure() { pin_failures_.fetch_add(1, std::memory_order_relaxed); }

    void RecordDiskRead(size_t bytes, bool wal) {
        disk_read_ops_.fetch_add(1, std::memory_order_relaxed);
        disk_read_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        if (wal) {
            wal_read_ops_.fetch_add(1, std::memory_order_relaxed);
            wal_read_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void RecordDiskWrite(size_t bytes, bool wal) {
        disk_write_ops_.fetch_add(1, std::memory_order_relaxed);
        disk_write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        if (wal) {
            wal_write_ops_.fetch_add(1, std::memory_order_relaxed);
            wal_write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    void RecordWalReset(uint64_t pause_ns, uint64_t dirty_pages, uint64_t discarded_bytes) {
        wal_reset_count_.fetch_add(1, std::memory_order_relaxed);
        wal_reset_dirty_pages_.fetch_add(dirty_pages, std::memory_order_relaxed);
        wal_reset_discarded_bytes_.fetch_add(discarded_bytes, std::memory_order_relaxed);
        uint64_t observed = wal_reset_max_pause_ns_.load(std::memory_order_relaxed);
        while (pause_ns > observed &&
               !wal_reset_max_pause_ns_.compare_exchange_weak(
                   observed, pause_ns, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    bool Write() const {
        std::ofstream output(path_, std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output << "{\n"
               << "  \"buffer_pool_pages\": " << buffer_pool_pages_ << ",\n"
               << "  \"buffer_pool_hits\": " << Load(buffer_pool_hits_) << ",\n"
               << "  \"buffer_pool_misses\": " << Load(buffer_pool_misses_) << ",\n"
               << "  \"buffer_pool_evictions\": " << Load(buffer_pool_evictions_) << ",\n"
               << "  \"dirty_flushes\": " << Load(dirty_flushes_) << ",\n"
               << "  \"pin_failures\": " << Load(pin_failures_) << ",\n"
               << "  \"disk_read_ops\": " << Load(disk_read_ops_) << ",\n"
               << "  \"disk_read_bytes\": " << Load(disk_read_bytes_) << ",\n"
               << "  \"disk_write_ops\": " << Load(disk_write_ops_) << ",\n"
               << "  \"disk_write_bytes\": " << Load(disk_write_bytes_) << ",\n"
               << "  \"wal_read_ops\": " << Load(wal_read_ops_) << ",\n"
               << "  \"wal_read_bytes\": " << Load(wal_read_bytes_) << ",\n"
               << "  \"wal_write_ops\": " << Load(wal_write_ops_) << ",\n"
               << "  \"wal_write_bytes\": " << Load(wal_write_bytes_) << ",\n"
               << "  \"wal_reset_count\": " << Load(wal_reset_count_) << ",\n"
               << "  \"wal_reset_max_pause_ns\": " << Load(wal_reset_max_pause_ns_) << ",\n"
               << "  \"wal_reset_dirty_pages\": " << Load(wal_reset_dirty_pages_) << ",\n"
               << "  \"wal_reset_discarded_bytes\": " << Load(wal_reset_discarded_bytes_) << "\n"
               << "}\n";
        output.flush();
        return output.good();
    }

   private:
    static uint64_t Load(const std::atomic<uint64_t> &counter) {
        return counter.load(std::memory_order_relaxed);
    }

    std::string path_;
    size_t buffer_pool_pages_;
    std::atomic<uint64_t> buffer_pool_hits_{0};
    std::atomic<uint64_t> buffer_pool_misses_{0};
    std::atomic<uint64_t> buffer_pool_evictions_{0};
    std::atomic<uint64_t> dirty_flushes_{0};
    std::atomic<uint64_t> pin_failures_{0};
    std::atomic<uint64_t> disk_read_ops_{0};
    std::atomic<uint64_t> disk_read_bytes_{0};
    std::atomic<uint64_t> disk_write_ops_{0};
    std::atomic<uint64_t> disk_write_bytes_{0};
    std::atomic<uint64_t> wal_read_ops_{0};
    std::atomic<uint64_t> wal_read_bytes_{0};
    std::atomic<uint64_t> wal_write_ops_{0};
    std::atomic<uint64_t> wal_write_bytes_{0};
    std::atomic<uint64_t> wal_reset_count_{0};
    std::atomic<uint64_t> wal_reset_max_pause_ns_{0};
    std::atomic<uint64_t> wal_reset_dirty_pages_{0};
    std::atomic<uint64_t> wal_reset_discarded_bytes_{0};
};
