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
#include <cstring>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
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
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> current_rec_;

    std::unique_ptr<RmRecord> join_records(const RmRecord *left_rec, const RmRecord *right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    void advance_to_next_match() {
        current_rec_.reset();
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                auto right_rec = right_->Next();
                auto joined = join_records(left_rec_.get(), right_rec.get());
                right_->nextTuple();
                if (eval_conds(cols_, joined.get(), fed_conds_)) {
                    current_rec_ = std::move(joined);
                    return;
                }
            }
            left_->nextTuple();
            if (left_->is_end()) {
                break;
            }
            left_rec_ = left_->Next();
            right_->beginTuple();
        }
        isend = true;
    }

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

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "NestedLoopJoinExecutor"; }

    bool is_end() const override { return isend; }

    void beginTuple() override {
        left_->beginTuple();
        isend = false;
        if (left_->is_end()) {
            isend = true;
            return;
        }
        left_rec_ = left_->Next();
        right_->beginTuple();
        advance_to_next_match();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_rec_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_rec_);
    }

    Rid &rid() override { return _abstract_rid; }
};
