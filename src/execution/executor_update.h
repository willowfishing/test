/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    bool executed_;

    struct RowChange {
        Rid rid{};
        std::shared_ptr<RmRecord> before;
        std::shared_ptr<RmRecord> after;
    };

    static double numeric_value(const char *data, ColType type) {
        if (type == TYPE_INT) return *reinterpret_cast<const int *>(data);
        if (type == TYPE_FLOAT) return *reinterpret_cast<const float *>(data);
        throw IncompatibleTypeError("numeric", coltype2str(type));
    }

    static double delta_value(const Value &value) {
        if (value.type == TYPE_INT) return value.int_val;
        if (value.type == TYPE_FLOAT) return value.float_val;
        throw IncompatibleTypeError("numeric", coltype2str(value.type));
    }

public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name,
                   std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids,
                   Context *context)
        : tab_(sm_manager->db_.get_table(tab_name)), conds_(std::move(conds)),
          fh_(sm_manager->fhs_.at(tab_name).get()), rids_(std::move(rids)),
          tab_name_(tab_name), set_clauses_(std::move(set_clauses)),
          sm_manager_(sm_manager), executed_(false) {
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (executed_) return nullptr;
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr) {
            throw InternalError("UPDATE requires a transaction context");
        }

        std::vector<RowChange> changes;
        std::unordered_set<TxnRowKey, TxnRowKeyHash> affected;
        for (const Rid &rid : rids_) {
            auto before = context_->txn_mgr_->get_visible_record(tab_name_, rid, context_->txn_);
            if (before == nullptr) continue;
            auto after = std::make_shared<RmRecord>(*before);
            for (auto &set_clause : set_clauses_) {
                auto target = tab_.get_col(set_clause.lhs.col_name);
                if (!set_clause.self_ref) {
                    if (set_clause.rhs.raw == nullptr) set_clause.rhs.init_raw(target->len);
                    std::memcpy(after->data + target->offset,
                                set_clause.rhs.raw->data, target->len);
                    continue;
                }
                auto source = tab_.get_col(set_clause.rhs_col.col_name);
                const double value = numeric_value(after->data + source->offset, source->type) +
                                     delta_value(set_clause.delta);
                if (target->type == TYPE_INT) {
                    const int converted = static_cast<int>(value);
                    std::memcpy(after->data + target->offset, &converted, sizeof(converted));
                } else if (target->type == TYPE_FLOAT) {
                    const float converted = static_cast<float>(value);
                    std::memcpy(after->data + target->offset, &converted, sizeof(converted));
                } else {
                    throw IncompatibleTypeError("numeric", coltype2str(target->type));
                }
            }
            changes.push_back({rid, before, after});
            affected.insert({tab_name_, rid});
        }

        std::vector<std::pair<Rid, std::shared_ptr<RmRecord>>> candidates;
        candidates.reserve(changes.size());
        for (const auto &change : changes) candidates.push_back({change.rid, change.after});
        context_->txn_mgr_->validate_unique_batch(tab_name_, candidates, affected,
                                                  context_->txn_);

        for (const auto &change : changes) {
            context_->txn_mgr_->register_update(tab_name_, change.rid,
                                                *change.before, *change.after,
                                                context_->txn_);
        }
        executed_ = true;
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
