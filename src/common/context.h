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

#include <memory>
#include <vector>

#include "transaction/transaction.h"
#include "transaction/concurrency/lock_manager.h"
#include "recovery/log_manager.h"

class TransactionManager;

// used for data_send
static int const_offset = -1;

class StatementScratch {
public:
    void reset() { record_cursor_ = 0; }

    std::shared_ptr<RmRecord> acquire_record(int len) {
        if (record_cursor_ == records_.size()) {
            records_.push_back(std::make_shared<RmRecord>(len));
        }
        auto record = records_[record_cursor_++];
        record->Resize(len);
        return record;
    }

private:
    std::vector<std::shared_ptr<RmRecord>> records_;
    size_t record_cursor_ = 0;
};

class Context {
public:
    Context (LockManager *lock_mgr, LogManager *log_mgr, 
            TransactionManager *txn_mgr, Transaction *txn, char *data_send = nullptr, int *offset = &const_offset,
            IsolationLevel *session_isolation = nullptr, StatementScratch *scratch = nullptr)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_mgr_(txn_mgr), txn_(txn),
          data_send_(data_send), offset_(offset), session_isolation_(session_isolation), scratch_(scratch) {
            ellipsis_ = false;
          }

    std::shared_ptr<RmRecord> acquire_template_raw_record(int len) {
        if (scratch_ == nullptr) {
            return std::make_shared<RmRecord>(len);
        }
        return scratch_->acquire_record(len);
    }

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    TransactionManager *txn_mgr_;
    Transaction *txn_;
    char *data_send_;
    int *offset_;
    IsolationLevel *session_isolation_;
    StatementScratch *scratch_;
    bool ellipsis_;
};
