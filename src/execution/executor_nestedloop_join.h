#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend;
    // Saved current left/right record data
    std::vector<char> left_buf_;
    std::vector<char> right_buf_;
    size_t left_len_;
    size_t right_len_;

    bool check_join() {
        if (fed_conds_.empty()) return true;
        for (auto &cond : fed_conds_) {
            auto lhs_col = get_col(cols_, cond.lhs_col);
            auto rhs_col = get_col(cols_, cond.rhs_col);
            char *lhs_data, *rhs_data;
            if (lhs_col->tab_name == left_->cols()[0].tab_name)
                lhs_data = left_buf_.data() + lhs_col->offset;
            else
                lhs_data = right_buf_.data() + (lhs_col->offset - left_len_);
            if (rhs_col->tab_name == left_->cols()[0].tab_name)
                rhs_data = left_buf_.data() + rhs_col->offset;
            else
                rhs_data = right_buf_.data() + (rhs_col->offset - left_len_);
            int cmp = 0;
            if (lhs_col->type == TYPE_INT)
                cmp = (*(int*)lhs_data > *(int*)rhs_data) - (*(int*)lhs_data < *(int*)rhs_data);
            else if (lhs_col->type == TYPE_FLOAT) {
                float a = *(float*)lhs_data, b = *(float*)rhs_data;
                cmp = (a > b) - (a < b);
            } else
                cmp = strncmp(lhs_data, rhs_data, std::min(lhs_col->len, rhs_col->len));
            bool ok = false;
            switch (cond.op) {
                case OP_EQ: ok = (cmp == 0); break;
                case OP_NE: ok = (cmp != 0); break;
                case OP_LT: ok = (cmp < 0); break;
                case OP_GT: ok = (cmp > 0); break;
                case OP_LE: ok = (cmp <= 0); break;
                case OP_GE: ok = (cmp >= 0); break;
            }
            if (!ok) return false;
        }
        return true;
    }

    bool save_current() {
        auto lr = left_->Next();
        if (!lr) return false;
        auto rr = right_->Next();
        if (!rr) return false;
        memcpy(left_buf_.data(), lr->data, left_len_);
        memcpy(right_buf_.data(), rr->data, right_len_);
        return check_join();
    }

    bool find_next_match() {
        while (!left_->is_end()) {
            right_->beginTuple();
            while (!right_->is_end()) {
                if (save_current()) return true;
                right_->nextTuple();
            }
            left_->nextTuple();
        }
        return false;
    }

public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        left_buf_.resize(left_len_);
        right_buf_.resize(right_len_);
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) col.offset += left_len_;
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta>& cols() const override { return cols_; }

    void beginTuple() override {
        left_->beginTuple();
        isend = false;
        if (!find_next_match()) isend = true;
    }

    void nextTuple() override {
        right_->nextTuple();
        // Try current right scan
        if (!right_->is_end() && save_current()) return;
        // Advance left and restart right
        left_->nextTuple();
        if (!find_next_match()) isend = true;
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_buf_.data(), left_len_);
        memcpy(rec->data + left_len_, right_buf_.data(), right_len_);
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
};
