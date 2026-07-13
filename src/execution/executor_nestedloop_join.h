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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    bool check_join_condition(const RmRecord &left_rec, const RmRecord &right_rec) {
        if (fed_conds_.empty()) return true;
        for (auto &cond : fed_conds_) {
            // Find the column in either left or right
            const ColMeta *lhs_col = nullptr;
            char *lhs_buf = nullptr;
            for (const auto &c : left_->cols()) {
                if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) {
                    lhs_col = &c;
                    lhs_buf = left_rec.data + c.offset;
                    break;
                }
            }
            if (!lhs_col) {
                for (const auto &c : right_->cols()) {
                    if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) {
                        lhs_col = &c;
                        lhs_buf = right_rec.data + c.offset;
                        break;
                    }
                }
            }
            if (!lhs_col) continue;

            char *rhs_buf = nullptr;
            const ColMeta *rhs_col = nullptr;
            for (const auto &c : left_->cols()) {
                if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) {
                    rhs_col = &c;
                    rhs_buf = left_rec.data + c.offset;
                    break;
                }
            }
            if (!rhs_col) {
                for (const auto &c : right_->cols()) {
                    if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) {
                        rhs_col = &c;
                        rhs_buf = right_rec.data + c.offset;
                        break;
                    }
                }
            }
            if (!rhs_col) continue;

            int cmp = SeqScanExecutor::compare_value(lhs_col->type, rhs_col->type, lhs_buf, rhs_buf,
                                                      lhs_col->len, rhs_col->len);
            if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
        }
        return true;
    }

    void beginTuple() override {
        left_->beginTuple();
        if (!left_->is_end()) {
            right_->beginTuple();
            // Find first matching pair
            while (!left_->is_end()) {
                while (!right_->is_end()) {
                    auto left_rec = left_->Next();
                    auto right_rec = right_->Next();
                    if (check_join_condition(*left_rec, *right_rec)) {
                        return;
                    }
                    right_->nextTuple();
                }
                left_->nextTuple();
                if (!left_->is_end()) {
                    right_->beginTuple();
                }
            }
        }
    }

    void nextTuple() override {
        right_->nextTuple();
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                auto left_rec = left_->Next();
                auto right_rec = right_->Next();
                if (check_join_condition(*left_rec, *right_rec)) {
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
    }

    bool is_end() const override {
        return left_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        auto join_rec = std::make_unique<RmRecord>(len_);
        memcpy(join_rec->data, left_rec->data, left_->tupleLen());
        memcpy(join_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return join_rec;
    }

    Rid &rid() override { return _abstract_rid; }
};