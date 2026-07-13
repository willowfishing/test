#pragma once

#include "executor_abstract.h"
#include "executor_seq_scan.h"

class FilterExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<Condition> conds_;
    std::unique_ptr<RmRecord> current_;
    bool is_end_{true};
    std::shared_ptr<RuntimeStat> runtime_;

    void seek_match() {
        while (!child_->is_end()) {
            auto rec = child_->Next();
            if (rec != nullptr && eval_conditions(conds_, child_->cols(), rec->data)) {
                current_ = std::move(rec);
                ++runtime_->rows;
                is_end_ = false;
                return;
            }
            child_->nextTuple();
        }
        current_.reset();
        is_end_ = true;
    }

   public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                   std::vector<Condition> conds,
                   std::shared_ptr<RuntimeStat> runtime)
        : child_(std::move(child)), conds_(std::move(conds)),
          runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
    }

    void beginTuple() override {
        child_->beginTuple();
        seek_match();
    }

    void nextTuple() override {
        if (is_end_) return;
        child_->nextTuple();
        seek_match();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || current_ == nullptr) return nullptr;
        return std::make_unique<RmRecord>(*current_);
    }

    size_t tupleLen() const override { return child_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return child_->cols(); }
    Rid &rid() override { return child_->rid(); }

    void bind_outer_tuple(const std::vector<ColMeta> &outer_cols,
                          const char *outer_data) override {
        child_->bind_outer_tuple(outer_cols, outer_data);
    }

};
