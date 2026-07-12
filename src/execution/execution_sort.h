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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<std::pair<ColMeta, bool>> order_cols_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        order_cols_.push_back({prev_->get_col_offset(sel_cols), is_desc});
        cursor_ = 0;
    }

    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<std::pair<TabCol, bool>> &order_cols) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        for (auto &order_col : order_cols) {
            order_cols_.push_back({prev_->get_col_offset(order_col.first), order_col.second});
        }
        cursor_ = 0;
    }

    void beginTuple() override { 
        tuples_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        std::sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (auto &order_col : order_cols_) {
                const auto &col = order_col.first;
                int cmp = compare_value(lhs->data + col.offset, rhs->data + col.offset, col.type, col.len);
                if (cmp != 0) {
                    return order_col.second ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, tuples_[cursor_]->data, len_);
        return rec;
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "SortExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
