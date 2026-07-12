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

#include <map>
#include <unordered_map>
#include <string>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "storage/buffer_pool_manager.h"
#include "system/sm_manager.h"

// Parsed log record for recovery
struct ParsedLogRecord {
    LogType log_type_;
    txn_id_t log_tid_;
    lsn_t lsn_;
    lsn_t prev_lsn_;
    uint32_t log_tot_len_;

    // Data for operation records
    RmRecord record_;       // INSERT: new record; DELETE/UPDATE: old record
    RmRecord new_record_;   // UPDATE: new record
    Rid rid_;
    std::string table_name_;
};

// Tracks dirty pages for redo
struct DPTEntry {
    std::string table_name_;
    int page_no_;
    std::vector<lsn_t> redo_logs_;
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
    }

    void analyze();
    void redo();
    void undo();

private:
    // Helpers
    void redo_insert(const ParsedLogRecord& rec);
    void redo_delete(const ParsedLogRecord& rec);
    void redo_update(const ParsedLogRecord& rec);

    LogBuffer buffer_;
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    SmManager* sm_manager_;

    // Recovery state
    std::vector<ParsedLogRecord> log_records_;        // in LSN order
    std::map<std::pair<std::string, int>, std::vector<lsn_t>> dpt_;  // (table, page_no) -> LSNs
    std::unordered_map<txn_id_t, std::string> txn_states_;  // true = committed, false = active/aborted
    std::map<std::string, std::vector<lsn_t>> committed_txn_records_;
};
