/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

static inline int compare_raw(const char *a, ColType type_a, int len_a,
                              const char *b, ColType type_b, int len_b) {
    if (type_a == TYPE_INT && type_b == TYPE_INT) {
        int ia = *reinterpret_cast<const int *>(a);
        int ib = *reinterpret_cast<const int *>(b);
        return (ia < ib) ? -1 : ((ia > ib) ? 1 : 0);
    }
    if (type_a == TYPE_FLOAT && type_b == TYPE_FLOAT) {
        float fa = *reinterpret_cast<const float *>(a);
        float fb = *reinterpret_cast<const float *>(b);
        return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
    }
    if (type_a == TYPE_INT && type_b == TYPE_FLOAT) {
        float fa = static_cast<float>(*reinterpret_cast<const int *>(a));
        float fb = *reinterpret_cast<const float *>(b);
        return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
    }
    if (type_a == TYPE_FLOAT && type_b == TYPE_INT) {
        float fa = *reinterpret_cast<const float *>(a);
        float fb = static_cast<float>(*reinterpret_cast<const int *>(b));
        return (fa < fb) ? -1 : ((fa > fb) ? 1 : 0);
    }
    const size_t max_a = static_cast<size_t>(len_a);
    const size_t max_b = static_cast<size_t>(len_b);
    size_t actual_a = 0;
    size_t actual_b = 0;
    while (actual_a < max_a && a[actual_a] != '\0') ++actual_a;
    while (actual_b < max_b && b[actual_b] != '\0') ++actual_b;
    const size_t common = std::min(actual_a, actual_b);
    const int prefix = common == 0 ? 0 : memcmp(a, b, common);
    if (prefix != 0) return prefix;
    return actual_a < actual_b ? -1 : (actual_a > actual_b ? 1 : 0);
}

static inline bool eval_condition(const Condition &cond,
                                  const std::vector<ColMeta> &cols,
                                  const char *data,
                                  const std::vector<ColMeta> *right_cols = nullptr,
                                  const char *right_data = nullptr) {
    const ColMeta *lhs_meta = nullptr;
    const char *lhs_base = data;
    for (const auto &c : cols) {
        if (c.tab_name == cond.lhs_col.tab_name &&
            c.name == cond.lhs_col.col_name) {
            lhs_meta = &c;
            break;
        }
    }
    if (lhs_meta == nullptr && right_cols != nullptr) {
        for (const auto &c : *right_cols) {
            if (c.tab_name == cond.lhs_col.tab_name &&
                c.name == cond.lhs_col.col_name) {
                lhs_meta = &c;
                lhs_base = right_data;
                break;
            }
        }
    }
    if (lhs_meta == nullptr) return false;

    const char *lhs_data = lhs_base + lhs_meta->offset;
    const char *rhs_raw = nullptr;
    ColType rhs_type{};
    int rhs_len = 0;

    if (cond.is_rhs_val) {
        rhs_raw = cond.rhs_val.raw->data;
        rhs_type = cond.rhs_val.type;
        rhs_len = lhs_meta->len;
    } else {
        const ColMeta *rhs_meta = nullptr;
        const char *rhs_base = data;
        for (const auto &c : cols) {
            if (c.tab_name == cond.rhs_col.tab_name &&
                c.name == cond.rhs_col.col_name) {
                rhs_meta = &c;
                break;
            }
        }
        if (rhs_meta == nullptr && right_cols != nullptr) {
            for (const auto &c : *right_cols) {
                if (c.tab_name == cond.rhs_col.tab_name &&
                    c.name == cond.rhs_col.col_name) {
                    rhs_meta = &c;
                    rhs_base = right_data;
                    break;
                }
            }
        }
        if (rhs_meta == nullptr) return false;
        rhs_raw = rhs_base + rhs_meta->offset;
        rhs_type = rhs_meta->type;
        rhs_len = rhs_meta->len;
    }

    const int cmp = compare_raw(lhs_data, lhs_meta->type, lhs_meta->len,
                                rhs_raw, rhs_type, rhs_len);
    switch (cond.op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

static inline bool eval_conditions(
    const std::vector<Condition> &conds, const std::vector<ColMeta> &cols,
    const char *data, const std::vector<ColMeta> *right_cols = nullptr,
    const char *right_data = nullptr) {
    for (const auto &cond : conds) {
        if (!eval_condition(cond, cols, data, right_cols, right_data)) {
            return false;
        }
    }
    return true;
}

class SeqScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_;
    std::string alias_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    Rid rid_{};
    std::vector<SnapshotRow> rows_;
    size_t pos_{0};
    bool is_end_{true};
    std::shared_ptr<RuntimeStat> runtime_;

    void seek_match() {
        while (pos_ < rows_.size()) {
            ++runtime_->rows;
            rid_ = rows_[pos_].rid;
            if (eval_conditions(conds_, cols_, rows_[pos_].record->data)) {
                is_end_ = false;
                if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                    context_->txn_mgr_->register_record_read(context_->txn_, tab_name_, rid_);
                }
                return;
            }
            ++pos_;
        }
        is_end_ = true;
    }

public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name,
                    std::string alias, std::vector<Condition> conds,
                    Context *context, std::shared_ptr<RuntimeStat> runtime)
        : tab_name_(std::move(tab_name)), alias_(std::move(alias)),
          conds_(std::move(conds)),
          fh_(sm_manager->fhs_.at(tab_name_).get()),
          cols_(sm_manager->db_.get_table(tab_name_).cols),
          runtime_(std::move(runtime)) {
        const std::string qualifier = alias_.empty() ? tab_name_ : alias_;
        for (auto &col : cols_) col.tab_name = qualifier;
        len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
    }

    void beginTuple() override {
        pos_ = 0;
        if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
            rows_ = context_->txn_mgr_->get_visible_rows(tab_name_, context_->txn_);
            context_->txn_mgr_->register_predicate_read(context_->txn_, tab_name_, conds_, cols_);
        } else {
            rows_.clear();
            for (RmScan scan(fh_); !scan.is_end(); scan.next()) {
                const Rid rid = scan.rid();
                rows_.push_back({rid, std::make_shared<RmRecord>(*fh_->get_record(rid, context_))});
            }
        }
        is_end_ = rows_.empty();
        if (!is_end_) seek_match();
    }

    void nextTuple() override {
        if (is_end_) return;
        ++pos_;
        seek_match();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || pos_ >= rows_.size()) return nullptr;
        return std::make_unique<RmRecord>(*rows_[pos_].record);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return rid_; }
};
