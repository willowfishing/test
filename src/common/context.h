/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "recovery/log_manager.h"
#include "transaction/concurrency/lock_manager.h"
#include "transaction/transaction.h"

class TransactionManager;

static int const_offset = -1;

class Context {
public:
    Context(LockManager *lock_mgr, LogManager *log_mgr, Transaction *txn,
            char *data_send = nullptr, int *offset = &const_offset,
            TransactionManager *txn_mgr = nullptr,
            IsolationLevel *session_isolation = nullptr)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),
          data_send_(data_send), offset_(offset), txn_mgr_(txn_mgr),
          session_isolation_(session_isolation) {
        ellipsis_ = false;
    }

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    Transaction *txn_;
    char *data_send_;
    int *offset_;
    bool ellipsis_;
    TransactionManager *txn_mgr_;
    IsolationLevel *session_isolation_;
};
