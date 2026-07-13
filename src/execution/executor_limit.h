#pragma once

#include <memory>
#include <utility>

#include "executor_abstract.h"

class LimitExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    size_t limit_;
    size_t emitted_{0};
    std::shared_ptr<RuntimeStat> runtime_;

   public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> prev, size_t limit,
                  std::shared_ptr<RuntimeStat> runtime)
        : prev_(std::move(prev)), limit_(limit), runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
    }

    void beginTuple() override {
        emitted_ = 0;
        prev_->beginTuple();
    }

    void nextTuple() override {
        if (is_end()) return;
        ++emitted_;
        prev_->nextTuple();
    }

    bool is_end() const override { return emitted_ >= limit_ || prev_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        auto result = prev_->Next();
        if (result) ++runtime_->rows;
        return result;
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    Rid &rid() override { return prev_->rid(); }
};
