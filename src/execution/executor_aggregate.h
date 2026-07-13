/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    struct GroupState {
        std::shared_ptr<RmRecord> first;
        std::vector<std::shared_ptr<RmRecord>> records;
    };

    struct EvalValue {
        ColType type{TYPE_INT};
        int int_val{0};
        float float_val{0};
        std::string str_val;
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectItem> select_items_;
    std::vector<TabCol> group_by_cols_;
    std::vector<HavingCondition> having_conds_;
    std::vector<ColMeta> cols_;
    size_t len_{0};
    std::vector<std::unique_ptr<RmRecord>> results_;
    size_t cursor_{0};
    bool built_{false};

    ColMeta input_col(const TabCol &target) const {
        const auto &prev_cols = prev_->cols();
        auto pos = std::find_if(prev_cols.begin(), prev_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == prev_cols.end()) {
            pos = std::find_if(prev_cols.begin(), prev_cols.end(), [&](const ColMeta &col) {
                return col.name == target.col_name;
            });
        }
        if (pos == prev_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
        return *pos;
    }

    ColType expr_type(const AggExpr &expr) const {
        if (expr.agg_type == AGG_COUNT) {
            return TYPE_INT;
        }
        if (expr.agg_type == AGG_AVG) {
            return TYPE_FLOAT;
        }
        return input_col(expr.col).type;
    }

    int expr_len(const AggExpr &expr) const {
        ColType type = expr_type(expr);
        if (type == TYPE_INT) {
            return sizeof(int);
        }
        if (type == TYPE_FLOAT) {
            return sizeof(float);
        }
        return input_col(expr.col).len;
    }

    std::string group_key(const RmRecord *rec) const {
        std::string key;
        for (auto &group_col : group_by_cols_) {
            auto col = input_col(group_col);
            key.append(rec->data + col.offset, col.len);
            key.push_back('\0');
        }
        return key;
    }

    EvalValue value_from_record(const RmRecord *rec, const ColMeta &col) const {
        EvalValue value;
        value.type = col.type;
        const char *raw = rec->data + col.offset;
        if (col.type == TYPE_INT) {
            value.int_val = *reinterpret_cast<const int *>(raw);
        } else if (col.type == TYPE_FLOAT) {
            value.float_val = *reinterpret_cast<const float *>(raw);
        } else {
            value.str_val = raw_string(raw, col.len);
        }
        return value;
    }

    int compare_values(const EvalValue &lhs, const EvalValue &rhs) const {
        if (lhs.type == TYPE_FLOAT || rhs.type == TYPE_FLOAT) {
            float left = lhs.type == TYPE_FLOAT ? lhs.float_val : static_cast<float>(lhs.int_val);
            float right = rhs.type == TYPE_FLOAT ? rhs.float_val : static_cast<float>(rhs.int_val);
            return (left > right) - (left < right);
        }
        if (lhs.type == TYPE_INT && rhs.type == TYPE_INT) {
            return (lhs.int_val > rhs.int_val) - (lhs.int_val < rhs.int_val);
        }
        return (lhs.str_val > rhs.str_val) - (lhs.str_val < rhs.str_val);
    }

    EvalValue eval_expr(const AggExpr &expr, const GroupState &group) const {
        EvalValue result;
        if (!expr.is_agg()) {
            if (group.first == nullptr) {
                return result;
            }
            return value_from_record(group.first.get(), input_col(expr.col));
        }

        result.type = expr_type(expr);
        if (expr.agg_type == AGG_COUNT) {
            result.int_val = static_cast<int>(group.records.size());
            return result;
        }

        auto col = input_col(expr.col);
        if (group.records.empty()) {
            return result;
        }

        if (expr.agg_type == AGG_SUM || expr.agg_type == AGG_AVG) {
            double sum = 0;
            for (auto &rec : group.records) {
                auto value = value_from_record(rec.get(), col);
                sum += value.type == TYPE_FLOAT ? value.float_val : value.int_val;
            }
            if (expr.agg_type == AGG_AVG) {
                result.float_val = static_cast<float>(sum / group.records.size());
            } else if (result.type == TYPE_FLOAT) {
                result.float_val = static_cast<float>(sum);
            } else {
                result.int_val = static_cast<int>(sum);
            }
            return result;
        }

        result = value_from_record(group.records.front().get(), col);
        for (size_t i = 1; i < group.records.size(); ++i) {
            auto value = value_from_record(group.records[i].get(), col);
            int cmp = compare_values(value, result);
            if ((expr.agg_type == AGG_MAX && cmp > 0) || (expr.agg_type == AGG_MIN && cmp < 0)) {
                result = value;
            }
        }
        return result;
    }

    EvalValue literal_to_value(const Value &literal, ColType expected) const {
        EvalValue value;
        value.type = expected;
        if (expected == TYPE_FLOAT) {
            value.float_val = literal.type == TYPE_FLOAT ? literal.float_val : static_cast<float>(literal.int_val);
        } else if (expected == TYPE_INT) {
            value.int_val = literal.type == TYPE_INT ? literal.int_val : static_cast<int>(literal.float_val);
        } else {
            value.str_val = literal.str_val;
        }
        return value;
    }

    bool pass_having(const GroupState &group) const {
        for (auto &cond : having_conds_) {
            auto lhs = eval_expr(cond.lhs, group);
            auto rhs = cond.is_rhs_val ? literal_to_value(cond.rhs_val, lhs.type) : eval_expr(cond.rhs_expr, group);
            if (!compare_result(compare_values(lhs, rhs), cond.op)) {
                return false;
            }
        }
        return true;
    }

    bool should_emit_empty_group() const {
        if (!group_by_cols_.empty() || select_items_.empty()) {
            return false;
        }
        return std::all_of(select_items_.begin(), select_items_.end(), [](const SelectItem &item) {
            return item.agg_type == AGG_COUNT;
        });
    }

    void write_value(RmRecord *rec, const ColMeta &col, const EvalValue &value) const {
        char *dst = rec->data + col.offset;
        if (col.type == TYPE_INT) {
            *reinterpret_cast<int *>(dst) = value.int_val;
        } else if (col.type == TYPE_FLOAT) {
            *reinterpret_cast<float *>(dst) = value.float_val;
        } else {
            memset(dst, 0, col.len);
            memcpy(dst, value.str_val.c_str(), std::min<int>(col.len, value.str_val.size()));
        }
    }

    void build_results() {
        if (built_) {
            return;
        }
        built_ = true;
        results_.clear();
        cursor_ = 0;

        std::unordered_map<std::string, size_t> group_pos;
        std::vector<GroupState> groups;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            std::string key = group_by_cols_.empty() ? std::string("__all__") : group_key(rec.get());
            auto it = group_pos.find(key);
            if (it == group_pos.end()) {
                group_pos[key] = groups.size();
                GroupState group;
                group.first = std::make_shared<RmRecord>(*rec);
                groups.push_back(std::move(group));
                it = group_pos.find(key);
            }
            groups[it->second].records.push_back(std::make_shared<RmRecord>(*rec));
        }

        if (groups.empty() && should_emit_empty_group()) {
            groups.push_back(GroupState{});
        }

        for (auto &group : groups) {
            if (!pass_having(group)) {
                continue;
            }
            auto out = std::make_unique<RmRecord>(len_);
            for (size_t i = 0; i < select_items_.size(); ++i) {
                write_value(out.get(), cols_[i], eval_expr(select_items_[i], group));
            }
            results_.push_back(std::move(out));
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelectItem> select_items,
                      std::vector<TabCol> group_by_cols, std::vector<HavingCondition> having_conds) {
        prev_ = std::move(prev);
        select_items_ = std::move(select_items);
        group_by_cols_ = std::move(group_by_cols);
        having_conds_ = std::move(having_conds);

        size_t curr_offset = 0;
        for (auto &item : select_items_) {
            ColMeta col;
            col.tab_name = "";
            col.name = item.display_name();
            col.type = expr_type(item);
            col.len = expr_len(item);
            col.offset = static_cast<int>(curr_offset);
            col.index = false;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "AggregateExecutor"; }

    bool is_end() const override { return cursor_ >= results_.size(); }

    void beginTuple() override {
        build_results();
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < results_.size()) {
            ++cursor_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*results_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }
};
