/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class TransactionManager;

class RecoveryManager {
public:
    RecoveryManager(DiskManager *disk_manager,
                    BufferPoolManager *buffer_pool_manager,
                    SmManager *sm_manager,
                    LogManager *log_manager,
                    TransactionManager *txn_manager)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager),
          sm_manager_(sm_manager), log_manager_(log_manager),
          txn_manager_(txn_manager) {}
    ~RecoveryManager();

    void analyze();
    void redo();
    void undo();

    void create_static_checkpoint(txn_id_t checkpoint_txn = INVALID_TXN_ID);

private:
    struct LogMeta {
        int64_t offset{0};
        uint32_t length{0};
        LogType type{LogType::begin};
        lsn_t lsn{INVALID_LSN};
        txn_id_t txn_id{INVALID_TXN_ID};
        bool committed{false};
        bool aborted{false};
    };

    std::unique_ptr<LogRecord> read_record(const LogMeta &meta) const;
    void map_log(int64_t size);
    void unmap_log();
    int64_t read_restart_offset() const;
    void write_restart_offset(int64_t offset) const;
    void apply_redo(const LogRecord &record);
    void apply_undo(const LogRecord &record);
    void finish_recovery();

    std::vector<LogMeta> logs_;
    std::unordered_set<txn_id_t> active_;
    int64_t scan_start_{0};
    int64_t valid_log_size_{0};
    lsn_t max_lsn_{INVALID_LSN};
    txn_id_t max_txn_id_{INVALID_TXN_ID};
    std::unordered_map<std::string, std::unordered_set<int>> affected_pages_;
    const char *mapped_log_{nullptr};
    size_t mapped_log_size_{0};

    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    SmManager *sm_manager_;
    LogManager *log_manager_;
    TransactionManager *txn_manager_;
};
