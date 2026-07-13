#pragma once

#include "executor_abstract.h"
#include "executor_seq_scan.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> conds_;
    bool is_end_{true};
    std::unique_ptr<RmRecord> current_left_;
    std::unique_ptr<RmRecord> current_right_;
    std::shared_ptr<RuntimeStat> runtime_;

    void find_next_match() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                current_right_ = right_->Next();
                if (current_right_ != nullptr &&
                    eval_conditions(conds_, left_->cols(), current_left_->data,
                                    &right_->cols(), current_right_->data)) {
                    is_end_ = false;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (left_->is_end()) break;
            current_left_ = left_->Next();
            right_->bind_outer_tuple(left_->cols(), current_left_->data);
            right_->beginTuple();
        }
        is_end_ = true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds,
                           std::shared_ptr<RuntimeStat> runtime)
        : left_(std::move(left)), right_(std::move(right)),
          len_(left_->tupleLen() + right_->tupleLen()),
          cols_(left_->cols()), conds_(std::move(conds)),
          runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) col.offset += left_->tupleLen();
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            is_end_ = true;
            return;
        }
        current_left_ = left_->Next();
        right_->bind_outer_tuple(left_->cols(), current_left_->data);
        right_->beginTuple();
        find_next_match();
    }

    void nextTuple() override {
        if (is_end_) return;
        right_->nextTuple();
        find_next_match();
    }

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) return nullptr;
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(rec->data, current_left_->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), current_right_->data,
               right_->tupleLen());
        ++runtime_->rows;
        return rec;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return _abstract_rid; }
};
