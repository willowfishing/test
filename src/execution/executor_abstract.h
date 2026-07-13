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

#include <algorithm>
#include <set>

#include "execution_defs.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    // CurrentTuple() returns an executor-owned view.  The pointer is valid only
    // until the next beginTuple()/nextTuple() call on that executor.
    struct TupleRef {
        const RmRecord *record = nullptr;
        std::unique_ptr<RmRecord> owner;

        explicit operator bool() const { return record != nullptr; }
    };

    struct TupleView {
        const RmRecord *record = nullptr;
        const std::vector<const char *> *cells = nullptr;

        explicit operator bool() const { return record != nullptr || cells != nullptr; }

        const char *cell_at(const ColMeta &col, size_t idx) const {
            if (cells != nullptr) {
                if (idx >= cells->size() || (*cells)[idx] == nullptr) {
                    throw RMDBError("invalid tuple view cell");
                }
                return (*cells)[idx];
            }
            if (record == nullptr || record->data == nullptr || col.offset < 0 || col.len < 0 ||
                col.offset + col.len > record->size) {
                throw RMDBError("invalid tuple record view");
            }
            return record->data + col.offset;
        }
    };

    struct TupleViewRef {
        const TupleView *view = nullptr;
        std::unique_ptr<RmRecord> owner;
        TupleView owned_view;

        explicit operator bool() const { return view != nullptr && static_cast<bool>(*view); }
    };

    Rid _abstract_rid;

    Context *context_;

    using EvalCondFn = bool (*)(const char *, const char *, int);

    static bool eval_int_eq(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) == *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_int_ne(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) != *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_int_lt(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) < *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_int_gt(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) > *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_int_le(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) <= *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_int_ge(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const int *>(lhs) >= *reinterpret_cast<const int *>(rhs);
    }
    static bool eval_float_eq(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) == *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_float_ne(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) != *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_float_lt(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) < *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_float_gt(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) > *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_float_le(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) <= *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_float_ge(const char *lhs, const char *rhs, int) {
        return *reinterpret_cast<const float *>(lhs) >= *reinterpret_cast<const float *>(rhs);
    }
    static bool eval_bytes_eq(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) == 0; }
    static bool eval_bytes_ne(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) != 0; }
    static bool eval_bytes_lt(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) < 0; }
    static bool eval_bytes_gt(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) > 0; }
    static bool eval_bytes_le(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) <= 0; }
    static bool eval_bytes_ge(const char *lhs, const char *rhs, int len) { return memcmp(lhs, rhs, len) >= 0; }

    static EvalCondFn condition_eval_fn(ColType type, CompOp op) {
        if (type == TYPE_INT) {
            switch (op) {
                case OP_EQ: return eval_int_eq;
                case OP_NE: return eval_int_ne;
                case OP_LT: return eval_int_lt;
                case OP_GT: return eval_int_gt;
                case OP_LE: return eval_int_le;
                case OP_GE: return eval_int_ge;
            }
        }
        if (type == TYPE_FLOAT) {
            switch (op) {
                case OP_EQ: return eval_float_eq;
                case OP_NE: return eval_float_ne;
                case OP_LT: return eval_float_lt;
                case OP_GT: return eval_float_gt;
                case OP_LE: return eval_float_le;
                case OP_GE: return eval_float_ge;
            }
        }
        switch (op) {
            case OP_EQ: return eval_bytes_eq;
            case OP_NE: return eval_bytes_ne;
            case OP_LT: return eval_bytes_lt;
            case OP_GT: return eval_bytes_gt;
            case OP_LE: return eval_bytes_le;
            case OP_GE: return eval_bytes_ge;
        }
        return nullptr;
    }

    struct CompiledCondition {
        size_t lhs_idx = 0;
        size_t rhs_idx = 0;
        ColMeta lhs_col;
        ColMeta rhs_col;
        CompOp op = OP_EQ;
        const char *rhs_value = nullptr;
        bool rhs_is_value = false;
        EvalCondFn eval_fn = nullptr;
    };

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual const RmRecord *CurrentTuple() const { return nullptr; }

    virtual const TupleView *CurrentTupleView() const {
        const RmRecord *record = CurrentTuple();
        if (record == nullptr) {
            return nullptr;
        }
        fallback_tuple_view_.record = record;
        fallback_tuple_view_.cells = nullptr;
        return &fallback_tuple_view_;
    }

    TupleRef ReadTuple() {
        TupleRef tuple;
        tuple.record = CurrentTuple();
        if (tuple.record == nullptr) {
            tuple.owner = Next();
            tuple.record = tuple.owner.get();
        }
        return tuple;
    }

    TupleViewRef ReadTupleView() {
        TupleViewRef tuple;
        tuple.view = CurrentTupleView();
        if (tuple.view == nullptr) {
            tuple.owner = Next();
            if (tuple.owner != nullptr) {
                tuple.owned_view.record = tuple.owner.get();
                tuple.owned_view.cells = nullptr;
                tuple.view = &tuple.owned_view;
            }
        }
        return tuple;
    }

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) const {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }

    size_t get_col_index(const std::vector<ColMeta> &rec_cols, const TabCol &target) const {
        auto pos = get_col(rec_cols, target);
        return static_cast<size_t>(pos - rec_cols.begin());
    }

    int compare_value(const char *lhs, const char *rhs, ColType type, int len) const {
        if (type == TYPE_INT) {
            int a = *reinterpret_cast<const int *>(lhs);
            int b = *reinterpret_cast<const int *>(rhs);
            return (a > b) - (a < b);
        }
        if (type == TYPE_FLOAT) {
            float a = *reinterpret_cast<const float *>(lhs);
            float b = *reinterpret_cast<const float *>(rhs);
            return (a > b) - (a < b);
        }
        return memcmp(lhs, rhs, len);
    }

    bool compare_result(int cmp, CompOp op) const {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    bool eval_conds(const std::vector<ColMeta> &rec_cols, const RmRecord *rec,
                    const std::vector<Condition> &conds) const {
        for (const auto &cond : conds) {
            auto lhs_col = get_col(rec_cols, cond.lhs_col);
            const char *lhs = rec->data + lhs_col->offset;
            const char *rhs = nullptr;
            if (cond.is_rhs_val) {
                rhs = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(rec_cols, cond.rhs_col);
                rhs = rec->data + rhs_col->offset;
            }
            if (!compare_result(compare_value(lhs, rhs, lhs_col->type, lhs_col->len), cond.op)) {
                return false;
            }
        }
        return true;
    }

    bool eval_conds_view(const std::vector<ColMeta> &rec_cols, const TupleView &view,
                         const std::vector<Condition> &conds) const {
        for (const auto &cond : conds) {
            auto lhs_col = get_col(rec_cols, cond.lhs_col);
            size_t lhs_idx = static_cast<size_t>(lhs_col - rec_cols.begin());
            const char *lhs = view.cell_at(*lhs_col, lhs_idx);
            const char *rhs = nullptr;
            if (cond.is_rhs_val) {
                rhs = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(rec_cols, cond.rhs_col);
                size_t rhs_idx = static_cast<size_t>(rhs_col - rec_cols.begin());
                rhs = view.cell_at(*rhs_col, rhs_idx);
            }
            if (!compare_result(compare_value(lhs, rhs, lhs_col->type, lhs_col->len), cond.op)) {
                return false;
            }
        }
        return true;
    }

    bool compile_conds(const std::vector<ColMeta> &rec_cols, const std::vector<Condition> &conds,
                       std::vector<CompiledCondition> *compiled) const {
        compiled->clear();
        compiled->reserve(conds.size());
        for (const auto &cond : conds) {
            auto lhs_col = get_col(rec_cols, cond.lhs_col);
            CompiledCondition item;
            item.lhs_idx = static_cast<size_t>(lhs_col - rec_cols.begin());
            item.lhs_col = *lhs_col;
            item.op = cond.op;
            item.rhs_is_value = cond.is_rhs_val;
            item.eval_fn = condition_eval_fn(lhs_col->type, cond.op);
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    compiled->clear();
                    return false;
                }
                item.rhs_value = cond.rhs_val.raw->data;
            } else {
                auto rhs_col = get_col(rec_cols, cond.rhs_col);
                if (rhs_col->type != lhs_col->type || rhs_col->len != lhs_col->len) {
                    compiled->clear();
                    return false;
                }
                item.rhs_idx = static_cast<size_t>(rhs_col - rec_cols.begin());
                item.rhs_col = *rhs_col;
            }
            compiled->push_back(item);
        }
        return true;
    }

    bool eval_compiled_conds_view(const TupleView &view, const std::vector<CompiledCondition> &conds) const {
        for (const auto &cond : conds) {
            const char *lhs = view.cell_at(cond.lhs_col, cond.lhs_idx);
            const char *rhs = cond.rhs_is_value ? cond.rhs_value : view.cell_at(cond.rhs_col, cond.rhs_idx);
            if (!cond.eval_fn(lhs, rhs, cond.lhs_col.len)) {
                return false;
            }
        }
        return true;
    }

    bool eval_compiled_conds_record(const RmRecord *rec, const std::vector<CompiledCondition> &conds) const {
        for (const auto &cond : conds) {
            const char *lhs = rec->data + cond.lhs_col.offset;
            const char *rhs = cond.rhs_is_value ? cond.rhs_value : rec->data + cond.rhs_col.offset;
            if (!cond.eval_fn(lhs, rhs, cond.lhs_col.len)) {
                return false;
            }
        }
        return true;
    }

    void materialize_tuple_view(const TupleView &view, const std::vector<ColMeta> &rec_cols, RmRecord *out,
                                size_t tuple_len) const {
        out->Resize(static_cast<int>(tuple_len));
        if (view.cells == nullptr && view.record != nullptr && view.record->data != nullptr &&
            static_cast<size_t>(view.record->size) >= tuple_len) {
            memcpy(out->data, view.record->data, tuple_len);
            return;
        }
        for (size_t i = 0; i < rec_cols.size(); ++i) {
            const auto &col = rec_cols[i];
            if (col.offset < 0 || col.len < 0 || static_cast<size_t>(col.offset + col.len) > tuple_len) {
                throw RMDBError("invalid materialized tuple column");
            }
            memcpy(out->data + col.offset, view.cell_at(col, i), col.len);
        }
    }

    void build_column_projection(const std::vector<ColMeta> &full_cols, const std::vector<TabCol> &required_cols,
                                 std::vector<ColMeta> *out_cols, std::vector<ColMeta> *source_cols,
                                 size_t *out_len) const {
        out_cols->clear();
        source_cols->clear();
        *out_len = 0;
        std::set<TabCol> seen;
        for (const auto &required : required_cols) {
            if (!seen.insert(required).second) {
                continue;
            }
            auto source = get_col(full_cols, required);
            ColMeta out_col = *source;
            out_col.offset = static_cast<int>(*out_len);
            *out_len += static_cast<size_t>(out_col.len);
            out_cols->push_back(out_col);
            source_cols->push_back(*source);
        }
    }

   private:
    mutable TupleView fallback_tuple_view_;
};
