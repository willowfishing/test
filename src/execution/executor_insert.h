/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <unordered_set>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Value> values_;
    RmFileHandle *fh_;
    std::string tab_name_;
    Rid rid_{};
    SmManager *sm_manager_;
    bool executed_{false};

public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name,
                   std::vector<Value> values, Context *context)
        : tab_(sm_manager->db_.get_table(tab_name)), values_(std::move(values)),
          fh_(sm_manager->fhs_.at(tab_name).get()), tab_name_(tab_name),
          sm_manager_(sm_manager) {
        if (values_.size() != tab_.cols.size()) throw InvalidValueCountError();
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (executed_) return nullptr;
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr) {
            throw InternalError("INSERT requires a transaction context");
        }
        RmRecord record(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &column = tab_.cols[i];
            auto &value = values_[i];
            if (column.type != value.type) {
                throw IncompatibleTypeError(coltype2str(column.type), coltype2str(value.type));
            }
            if (value.raw == nullptr) value.init_raw(column.len);
            std::memcpy(record.data + column.offset, value.raw->data, column.len);
        }

        const Rid probe{-1000000000 - static_cast<int>(context_->txn_->get_transaction_id()),
                        -1000000000};
        auto shared = std::make_shared<RmRecord>(record);
        context_->txn_mgr_->validate_unique_batch(
            tab_name_, {{probe, shared}}, {}, context_->txn_);
        rid_ = context_->txn_mgr_->register_insert(tab_name_, record, context_->txn_);
        executed_ = true;
        return nullptr;
    }

    Rid &rid() override { return rid_; }
};
