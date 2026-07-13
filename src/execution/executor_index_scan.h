/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    std::string alias_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    std::vector<Condition> lookup_conds_;
    std::vector<Condition> active_conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;

    Rid rid_;
    std::unique_ptr<IxScan> scan_;
    bool is_end_;
    IxIndexHandle *ih_;
    SmManager *sm_manager_;
    std::shared_ptr<RuntimeStat> runtime_;
    bool lookup_bound_{false};
    bool lookup_impossible_{false};

    static void set_min_value(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            const int value = std::numeric_limits<int>::lowest();
            memcpy(dest, &value, sizeof(value));
        } else if (col.type == TYPE_FLOAT) {
            const float value = std::numeric_limits<float>::lowest();
            memcpy(dest, &value, sizeof(value));
        } else {
            memset(dest, 0, col.len);
        }
    }

    static void set_max_value(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            const int value = std::numeric_limits<int>::max();
            memcpy(dest, &value, sizeof(value));
        } else if (col.type == TYPE_FLOAT) {
            const float value = std::numeric_limits<float>::max();
            memcpy(dest, &value, sizeof(value));
        } else {
            memset(dest, 0xff, col.len);
        }
    }

    const Condition *find_equality(const ColMeta &col) const {
        for (const auto &cond : active_conds_) {
            if (cond.is_rhs_val && cond.lhs_col.col_name == col.name && cond.op == OP_EQ) {
                return &cond;
            }
        }
        return nullptr;
    }

    const Condition *find_tight_lower(const ColMeta &col) const {
        const Condition *best = nullptr;
        for (const auto &cond : active_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.col_name != col.name ||
                (cond.op != OP_GT && cond.op != OP_GE)) {
                continue;
            }
            if (best == nullptr) {
                best = &cond;
                continue;
            }
            const int cmp = ix_compare(cond.rhs_val.raw->data, best->rhs_val.raw->data, col.type, col.len);
            if (cmp > 0 || (cmp == 0 && cond.op == OP_GT && best->op == OP_GE)) {
                best = &cond;
            }
        }
        return best;
    }

    const Condition *find_tight_upper(const ColMeta &col) const {
        const Condition *best = nullptr;
        for (const auto &cond : active_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.col_name != col.name ||
                (cond.op != OP_LT && cond.op != OP_LE)) {
                continue;
            }
            if (best == nullptr) {
                best = &cond;
                continue;
            }
            const int cmp = ix_compare(cond.rhs_val.raw->data, best->rhs_val.raw->data, col.type, col.len);
            if (cmp < 0 || (cmp == 0 && cond.op == OP_LT && best->op == OP_LE)) {
                best = &cond;
            }
        }
        return best;
    }

    void fill_suffix(std::vector<char> &key, size_t first_col, bool maximum) const {
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            if (i >= first_col) {
                if (maximum) {
                    set_max_value(key.data() + offset, index_meta_.cols[i]);
                } else {
                    set_min_value(key.data() + offset, index_meta_.cols[i]);
                }
            }
            offset += index_meta_.cols[i].len;
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::string alias,
                      std::vector<Condition> conds,
                      std::vector<std::string> index_col_names,
                      std::vector<Condition> lookup_conds, Context *context,
                      std::shared_ptr<RuntimeStat> runtime)
        : tab_name_(std::move(tab_name)),
          alias_(std::move(alias)),
          tab_(sm_manager->db_.get_table(tab_name_)),
          conds_(std::move(conds)),
          lookup_conds_(std::move(lookup_conds)),
          fh_(sm_manager->fhs_.at(tab_name_).get()),
          cols_(tab_.cols),
          len_(cols_.back().offset + cols_.back().len),
          index_col_names_(std::move(index_col_names)),
          index_meta_(*tab_.get_index_meta(index_col_names_)),
          is_end_(true),
          sm_manager_(sm_manager),
          runtime_(std::move(runtime)) {
        context_ = context;
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        const std::string qualifier = alias_.empty() ? tab_name_ : alias_;
        for (auto &col : cols_) col.tab_name = qualifier;
        const std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        ih_ = sm_manager_->ihs_.at(ix_name).get();

        const std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };
        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != qualifier) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == qualifier);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        for (auto &cond : lookup_conds_) {
            if (cond.lhs_col.tab_name != qualifier) {
                if (cond.is_rhs_val || cond.rhs_col.tab_name != qualifier) {
                    throw InternalError("Invalid parameterized index predicate");
                }
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                throw InternalError("Parameterized index lookup requires an equality column predicate");
            }
        }
        active_conds_ = conds_;
        fed_conds_ = conds_;
    }

    // Backward-compatible constructor used by ordinary constant-bound index
    // scans in DELETE/UPDATE and single-table SELECT paths.
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::string alias,
                      std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context,
                      std::shared_ptr<RuntimeStat> runtime)
        : IndexScanExecutor(sm_manager, std::move(tab_name), std::move(alias),
                            std::move(conds), std::move(index_col_names), {},
                            context, std::move(runtime)) {}

    void bind_outer_tuple(const std::vector<ColMeta> &outer_cols,
                          const char *outer_data) override {
        if (lookup_conds_.empty()) return;

        active_conds_ = conds_;
        lookup_bound_ = true;
        lookup_impossible_ = false;

        for (const auto &lookup : lookup_conds_) {
            const auto inner_it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                return col.tab_name == lookup.lhs_col.tab_name &&
                       col.name == lookup.lhs_col.col_name;
            });
            const auto outer_it = std::find_if(outer_cols.begin(), outer_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == lookup.rhs_col.tab_name &&
                       col.name == lookup.rhs_col.col_name;
            });
            if (inner_it == cols_.end() || outer_it == outer_cols.end()) {
                throw InternalError("Failed to bind parameterized index predicate");
            }

            Condition bound = lookup;
            bound.is_rhs_val = true;
            bound.rhs_col = {};
            const char *raw = outer_data + outer_it->offset;

            if (inner_it->type == TYPE_INT) {
                int value = 0;
                if (outer_it->type == TYPE_INT) {
                    value = *reinterpret_cast<const int *>(raw);
                } else if (outer_it->type == TYPE_FLOAT) {
                    const float source = *reinterpret_cast<const float *>(raw);
                    const double promoted = static_cast<double>(source);
                    if (!std::isfinite(source) ||
                        promoted < static_cast<double>(std::numeric_limits<int>::lowest()) ||
                        promoted > static_cast<double>(std::numeric_limits<int>::max()) ||
                        std::trunc(promoted) != promoted) {
                        lookup_impossible_ = true;
                        break;
                    }
                    value = static_cast<int>(promoted);
                } else {
                    lookup_impossible_ = true;
                    break;
                }
                bound.rhs_val.set_int(value);
            } else if (inner_it->type == TYPE_FLOAT) {
                float value = 0;
                if (outer_it->type == TYPE_FLOAT) {
                    value = *reinterpret_cast<const float *>(raw);
                } else if (outer_it->type == TYPE_INT) {
                    value = static_cast<float>(*reinterpret_cast<const int *>(raw));
                } else {
                    lookup_impossible_ = true;
                    break;
                }
                bound.rhs_val.set_float(value);
            } else if (inner_it->type == TYPE_STRING && outer_it->type == TYPE_STRING) {
                size_t actual = 0;
                while (actual < static_cast<size_t>(outer_it->len) && raw[actual] != '\0') ++actual;
                if (actual > static_cast<size_t>(inner_it->len)) {
                    lookup_impossible_ = true;
                    break;
                }
                bound.rhs_val.set_str(std::string(raw, actual));
            } else {
                lookup_impossible_ = true;
                break;
            }
            bound.rhs_val.init_raw(inner_it->len);
            active_conds_.push_back(std::move(bound));
        }
    }


    void beginTuple() override {
        if (!lookup_conds_.empty() && (!lookup_bound_ || lookup_impossible_)) {
            is_end_ = true;
            scan_.reset();
            return;
        }
        std::vector<char> lower_key(index_meta_.col_tot_len);
        std::vector<char> upper_key(index_meta_.col_tot_len);
        fill_suffix(lower_key, 0, false);
        fill_suffix(upper_key, 0, true);

        bool usable = false;
        bool empty_range = false;
        bool lower_strict = false;
        bool upper_strict = false;
        int key_offset = 0;

        for (size_t col_no = 0; col_no < index_meta_.cols.size(); ++col_no) {
            const auto &col = index_meta_.cols[col_no];
            const Condition *eq = find_equality(col);
            if (eq != nullptr) {
                memcpy(lower_key.data() + key_offset, eq->rhs_val.raw->data, col.len);
                memcpy(upper_key.data() + key_offset, eq->rhs_val.raw->data, col.len);
                usable = true;
                key_offset += col.len;
                continue;
            }

            const Condition *lower = find_tight_lower(col);
            const Condition *upper = find_tight_upper(col);
            if (lower == nullptr && upper == nullptr) {
                break;
            }
            usable = true;

            if (lower != nullptr && upper != nullptr) {
                const int cmp = ix_compare(lower->rhs_val.raw->data, upper->rhs_val.raw->data, col.type, col.len);
                if (cmp > 0 || (cmp == 0 && (lower->op == OP_GT || upper->op == OP_LT))) {
                    empty_range = true;
                }
            }

            if (lower != nullptr) {
                memcpy(lower_key.data() + key_offset, lower->rhs_val.raw->data, col.len);
                lower_strict = lower->op == OP_GT;
                // For (prefix, x) > value, every key with x == value must be skipped.
                fill_suffix(lower_key, col_no + 1, lower_strict);
            }
            if (upper != nullptr) {
                memcpy(upper_key.data() + key_offset, upper->rhs_val.raw->data, col.len);
                upper_strict = upper->op == OP_LT;
                // For (prefix, x) < value, stop before the first key with x == value.
                fill_suffix(upper_key, col_no + 1, !upper_strict);
            }
            break;  // The first range column terminates the usable left prefix.
        }

        if (!usable || empty_range) {
            is_end_ = true;
            scan_.reset();
            return;
        }

        const Iid lower = lower_strict ? ih_->upper_bound(lower_key.data()) : ih_->lower_bound(lower_key.data());
        const Iid upper = upper_strict ? ih_->lower_bound(upper_key.data()) : ih_->upper_bound(upper_key.data());
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        is_end_ = scan_->is_end();
        if (!is_end_) skip_non_matching();
    }

    void nextTuple() override {
        if (is_end_) return;
        scan_->next();
        skip_non_matching();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override { return fh_->get_record(rid_, context_); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }

   private:
    void skip_non_matching() {
        while (!scan_->is_end()) {
            ++runtime_->rows;
            const Rid candidate = scan_->rid();
            auto rec = fh_->get_record(candidate, context_);
            if (eval_conditions(active_conds_, cols_, rec->data)) {
                rid_ = candidate;
                return;
            }
            scan_->next();
        }
        is_end_ = true;
    }
};
