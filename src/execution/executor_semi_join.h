#pragma once

#include "executor_abstract.h"

class SemiJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    std::vector<Condition> conds_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> combined_cols_;
    size_t len_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_;

   public:
    SemiJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                     std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        conds_ = std::move(conds);
        cols_ = left_->cols();
        combined_cols_ = cols_;
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
            combined_cols_.push_back(col);
        }
        len_ = left_->tupleLen();
        cursor_ = 0;
    }

    void beginTuple() override {
        tuples_.clear();
        for (left_->beginTuple(); !left_->is_end(); left_->nextTuple()) {
            auto left_rec = left_->Next();
            bool matched = false;
            for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
                auto right_rec = right_->Next();
                auto combined = std::make_unique<RmRecord>(left_->tupleLen() + right_->tupleLen());
                memcpy(combined->data, left_rec->data, left_->tupleLen());
                memcpy(combined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
                if (eval_conds(combined_cols_, combined.get(), conds_)) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                auto rec = std::make_unique<RmRecord>(len_);
                memcpy(rec->data, left_rec->data, len_);
                tuples_.push_back(std::move(rec));
            }
        }
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "SemiJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, tuples_[cursor_]->data, len_);
        return rec;
    }
};
