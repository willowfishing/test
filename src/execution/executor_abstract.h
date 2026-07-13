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
#include <cstring>

#include "execution_defs.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

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

    virtual ColMeta get_col_offset(const TabCol &target) {
        const auto &rec_cols = cols();
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return (col.tab_name == target.tab_name && col.name == target.col_name) ||
                   (target.tab_name.empty() && col.name == target.col_name);
        });
        if (pos == rec_cols.end()) {
            pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
                return col.name == target.col_name;
            });
        }
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return *pos;
    };

    static std::string raw_string(const char *data, int len) {
        std::string value(data, len);
        value.resize(strlen(value.c_str()));
        return value;
    }

    static int compare_raw_values(ColType type, const char *lhs, const char *rhs, int len) {
        if (type == TYPE_INT) {
            int left = *reinterpret_cast<const int *>(lhs);
            int right = *reinterpret_cast<const int *>(rhs);
            return (left > right) - (left < right);
        }
        if (type == TYPE_FLOAT) {
            float left = *reinterpret_cast<const float *>(lhs);
            float right = *reinterpret_cast<const float *>(rhs);
            return (left > right) - (left < right);
        }
        std::string left = raw_string(lhs, len);
        std::string right = raw_string(rhs, len);
        return (left > right) - (left < right);
    }

    static bool compare_result(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ:
                return cmp == 0;
            case OP_NE:
                return cmp != 0;
            case OP_LT:
                return cmp < 0;
            case OP_GT:
                return cmp > 0;
            case OP_LE:
                return cmp <= 0;
            case OP_GE:
                return cmp >= 0;
        }
        return false;
    }

    static bool eval_cond(const std::vector<ColMeta> &rec_cols, const RmRecord *rec, const Condition &cond) {
        auto lhs_col = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
        });
        if (lhs_col == rec_cols.end()) {
            throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
        }
        const char *lhs = rec->data + lhs_col->offset;
        const char *rhs = nullptr;
        if (cond.is_rhs_val) {
            rhs = cond.rhs_val.raw->data;
        } else {
            auto rhs_col = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
            });
            if (rhs_col == rec_cols.end()) {
                throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
            }
            rhs = rec->data + rhs_col->offset;
        }
        return compare_result(compare_raw_values(lhs_col->type, lhs, rhs, lhs_col->len), cond.op);
    }

    static bool eval_conds(const std::vector<ColMeta> &rec_cols, const RmRecord *rec,
                           const std::vector<Condition> &conds) {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, rec, cond)) {
                return false;
            }
        }
        return true;
    }

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};
