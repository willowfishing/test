/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <memory>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    int limit_{-1};
    int emitted_{0};

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, int limit) {
        prev_ = std::move(prev);
        limit_ = limit;
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    std::string getType() override { return "LimitExecutor"; }

    bool is_end() const override { return (limit_ >= 0 && emitted_ >= limit_) || prev_->is_end(); }

    void beginTuple() override {
        emitted_ = 0;
        prev_->beginTuple();
    }

    void nextTuple() override {
        if (!is_end()) {
            ++emitted_;
            prev_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return prev_->Next();
    }

    Rid &rid() override { return prev_->rid(); }
};
