#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    struct Group {
        std::unique_ptr<RmRecord> representative;
        std::vector<std::unique_ptr<RmRecord>> rows;
    };

    struct EvalValue {
        ColType type{TYPE_INT};
        int int_value{0};
        float float_value{0};
        std::string string_value;
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectItemInfo> select_items_;
    std::vector<TabCol> group_cols_;
    std::vector<HavingCondition> having_conds_;
    std::vector<ColMeta> input_cols_;
    std::vector<ColMeta> cols_;
    std::vector<size_t> group_idxs_;
    size_t len_{0};
    std::vector<std::unique_ptr<RmRecord>> results_;
    size_t pos_{0};
    std::shared_ptr<RuntimeStat> runtime_;

    size_t find_input_idx(const TabCol &target) const {
        for (size_t i = 0; i < input_cols_.size(); ++i) {
            if (input_cols_[i].tab_name == target.tab_name &&
                input_cols_[i].name == target.col_name) return i;
        }
        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }

    EvalValue read_col(const RmRecord &record, const TabCol &target) const {
        const auto &col = input_cols_[find_input_idx(target)];
        EvalValue value;
        value.type = col.type;
        const char *data = record.data + col.offset;
        if (col.type == TYPE_INT) value.int_value = *reinterpret_cast<const int *>(data);
        else if (col.type == TYPE_FLOAT) value.float_value = *reinterpret_cast<const float *>(data);
        else {
            value.string_value.assign(data, col.len);
            value.string_value.resize(strlen(value.string_value.c_str()));
        }
        return value;
    }

    EvalValue evaluate(const QueryExpr &expr, const Group &group) const {
        if (!expr.is_aggregate) {
            if (!group.representative) throw RMDBError("Empty group has no plain column value");
            return read_col(*group.representative, expr.col);
        }

        EvalValue result;
        if (expr.agg_type == AggFuncType::COUNT) {
            result.type = TYPE_INT;
            result.int_value = static_cast<int>(group.rows.size());
            return result;
        }

        const auto &source = input_cols_[find_input_idx(expr.col)];
        result.type = expr.agg_type == AggFuncType::AVG ? TYPE_FLOAT : source.type;
        if (group.rows.empty()) return result;

        if (source.type == TYPE_INT) {
            long long sum = 0;
            int best = *reinterpret_cast<const int *>(group.rows.front()->data + source.offset);
            for (const auto &row : group.rows) {
                const int value = *reinterpret_cast<const int *>(row->data + source.offset);
                sum += value;
                if (expr.agg_type == AggFuncType::MAX && value > best) best = value;
                if (expr.agg_type == AggFuncType::MIN && value < best) best = value;
            }
            if (expr.agg_type == AggFuncType::SUM) result.int_value = static_cast<int>(sum);
            else if (expr.agg_type == AggFuncType::AVG) {
                result.float_value = static_cast<float>(sum) / static_cast<float>(group.rows.size());
            } else result.int_value = best;
        } else {
            double sum = 0;
            float best = *reinterpret_cast<const float *>(group.rows.front()->data + source.offset);
            for (const auto &row : group.rows) {
                const float value = *reinterpret_cast<const float *>(row->data + source.offset);
                sum += value;
                if (expr.agg_type == AggFuncType::MAX && value > best) best = value;
                if (expr.agg_type == AggFuncType::MIN && value < best) best = value;
            }
            if (expr.agg_type == AggFuncType::SUM) result.float_value = static_cast<float>(sum);
            else if (expr.agg_type == AggFuncType::AVG) {
                result.float_value = static_cast<float>(sum / static_cast<double>(group.rows.size()));
            } else result.float_value = best;
        }
        return result;
    }

    static EvalValue from_literal(const Value &value) {
        EvalValue result;
        result.type = value.type;
        if (value.type == TYPE_INT) result.int_value = value.int_val;
        else if (value.type == TYPE_FLOAT) result.float_value = value.float_val;
        else result.string_value = value.str_val;
        return result;
    }

    static int compare_values(const EvalValue &lhs, const EvalValue &rhs) {
        const bool lhs_numeric = lhs.type == TYPE_INT || lhs.type == TYPE_FLOAT;
        const bool rhs_numeric = rhs.type == TYPE_INT || rhs.type == TYPE_FLOAT;
        if (lhs_numeric && rhs_numeric) {
            const double a = lhs.type == TYPE_INT ? lhs.int_value : lhs.float_value;
            const double b = rhs.type == TYPE_INT ? rhs.int_value : rhs.float_value;
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        return lhs.string_value < rhs.string_value ? -1 :
               (lhs.string_value > rhs.string_value ? 1 : 0);
    }

    bool passes_having(const Group &group) const {
        for (const auto &cond : having_conds_) {
            const EvalValue lhs = evaluate(cond.lhs, group);
            const EvalValue rhs = cond.is_rhs_val ? from_literal(cond.rhs_val)
                                                  : evaluate(cond.rhs_expr, group);
            const int cmp = compare_values(lhs, rhs);
            bool ok = false;
            switch (cond.op) {
                case OP_EQ: ok = cmp == 0; break;
                case OP_NE: ok = cmp != 0; break;
                case OP_LT: ok = cmp < 0; break;
                case OP_GT: ok = cmp > 0; break;
                case OP_LE: ok = cmp <= 0; break;
                case OP_GE: ok = cmp >= 0; break;
            }
            if (!ok) return false;
        }
        return true;
    }

    void write_value(RmRecord &record, const ColMeta &meta, const EvalValue &value) const {
        char *dest = record.data + meta.offset;
        if (meta.type == TYPE_INT) *reinterpret_cast<int *>(dest) = value.int_value;
        else if (meta.type == TYPE_FLOAT) *reinterpret_cast<float *>(dest) = value.float_value;
        else {
            memset(dest, 0, meta.len);
            memcpy(dest, value.string_value.data(),
                   std::min(static_cast<size_t>(meta.len), value.string_value.size()));
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev,
                      std::vector<SelectItemInfo> select_items,
                      std::vector<TabCol> group_cols,
                      std::vector<HavingCondition> having_conds,
                      std::shared_ptr<RuntimeStat> runtime)
        : prev_(std::move(prev)), select_items_(std::move(select_items)),
          group_cols_(std::move(group_cols)), having_conds_(std::move(having_conds)),
          input_cols_(prev_->cols()), runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        for (const auto &group_col : group_cols_) group_idxs_.push_back(find_input_idx(group_col));

        size_t offset = 0;
        for (const auto &item : select_items_) {
            ColMeta meta{};
            meta.tab_name = "";
            meta.name = item.output_name;
            if (!item.expr.is_aggregate) {
                meta = input_cols_[find_input_idx(item.expr.col)];
                meta.tab_name = "";
                meta.name = item.output_name;
            } else if (item.expr.agg_type == AggFuncType::COUNT) {
                meta.type = TYPE_INT;
                meta.len = sizeof(int);
            } else if (item.expr.agg_type == AggFuncType::AVG) {
                meta.type = TYPE_FLOAT;
                meta.len = sizeof(float);
            } else {
                const auto &source = input_cols_[find_input_idx(item.expr.col)];
                meta.type = source.type;
                meta.len = source.len;
            }
            meta.offset = static_cast<int>(offset);
            meta.index = false;
            offset += meta.len;
            cols_.push_back(std::move(meta));
        }
        len_ = offset;
    }

    void beginTuple() override {
        results_.clear();
        pos_ = 0;
        std::vector<Group> groups;
        std::unordered_map<std::string, size_t> group_map;
        if (group_cols_.empty()) groups.emplace_back();

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto row = prev_->Next();
            if (!row) continue;
            size_t group_index = 0;
            if (!group_cols_.empty()) {
                std::string key;
                for (size_t idx : group_idxs_) {
                    const auto &col = input_cols_[idx];
                    key.append(row->data + col.offset, col.len);
                }
                auto [it, inserted] = group_map.emplace(key, groups.size());
                if (inserted) groups.emplace_back();
                group_index = it->second;
            }
            auto &group = groups[group_index];
            if (!group.representative) group.representative = std::make_unique<RmRecord>(*row);
            group.rows.push_back(std::move(row));
        }

        for (const auto &group : groups) {
            if (!passes_having(group)) continue;
            auto output = std::make_unique<RmRecord>(static_cast<int>(len_));
            memset(output->data, 0, len_);
            for (size_t i = 0; i < select_items_.size(); ++i) {
                write_value(*output, cols_[i], evaluate(select_items_[i].expr, group));
            }
            results_.push_back(std::move(output));
        }
    }

    void nextTuple() override {
        if (pos_ < results_.size()) ++pos_;
    }
    bool is_end() const override { return pos_ >= results_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        ++runtime_->rows;
        return std::make_unique<RmRecord>(*results_[pos_]);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return _abstract_rid; }
};
