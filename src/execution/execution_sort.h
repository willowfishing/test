/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;
    std::vector<ColMeta> cols_;
    size_t len_{0};
    bool is_desc_{false};
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_{0};
    bool built_{false};

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_col, bool is_desc) {
        prev_ = std::move(prev);
        sort_col_ = prev_->get_col_offset(sel_col);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        is_desc_ = is_desc;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "SortExecutor"; }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    void beginTuple() override {
        if (!built_) {
            tuples_.clear();
            for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
                auto rec = prev_->Next();
                if (rec != nullptr) {
                    tuples_.push_back(std::move(rec));
                }
            }
            std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
                int cmp = compare_raw_values(sort_col_.type, lhs->data + sort_col_.offset,
                                             rhs->data + sort_col_.offset, sort_col_.len);
                return is_desc_ ? cmp > 0 : cmp < 0;
            });
            built_ = true;
        }
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            ++cursor_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }
};
