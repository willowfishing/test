/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;
    bool executed_;

public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name,
                   std::vector<Condition> conds, std::vector<Rid> rids,
                   Context *context)
        : tab_(sm_manager->db_.get_table(tab_name)), conds_(std::move(conds)),
          fh_(sm_manager->fhs_.at(tab_name).get()), rids_(std::move(rids)),
          tab_name_(tab_name), sm_manager_(sm_manager), executed_(false) {
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (executed_) return nullptr;
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr) {
            throw InternalError("DELETE requires a transaction context");
        }
        for (const Rid &rid : rids_) {
            auto record = context_->txn_mgr_->get_visible_record(tab_name_, rid, context_->txn_);
            if (record == nullptr) continue;
            context_->txn_mgr_->register_delete(tab_name_, rid, *record, context_->txn_);
        }
        executed_ = true;
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
