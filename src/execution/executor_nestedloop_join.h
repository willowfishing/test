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
    std::unique_ptr<RmRecord> left_record_;

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
    const std::vector<ColMeta>& cols() const override { return cols_; }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        isend = false;
        // Get first left record
        left_record_ = left_->Next();
        if (left_record_ == nullptr) {
            isend = true;
            return;
        }
        right_->beginTuple();
    }

    void nextTuple() override {
        // Try to advance right
        right_->nextTuple();
        if (!right_->is_end()) return;  // still have right records
        
        // Right exhausted, advance left
        left_->nextTuple();
        left_record_ = left_->Next();
        if (left_record_ == nullptr) {
            isend = true;
            return;
        }
        right_->beginTuple();  // restart right scan
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend || left_record_ == nullptr) return nullptr;
        auto right_rec = right_->Next();
        if (right_rec == nullptr) return nullptr;
        
        // Check join condition
        if (!fed_conds_.empty()) {
            bool match = true;
            for (auto &cond : fed_conds_) {
                auto lhs_col = get_col(cols_, cond.lhs_col);
                auto rhs_col = get_col(cols_, cond.rhs_col);
                char *lhs_data = nullptr;
                char *rhs_data = nullptr;
                if (lhs_col->tab_name == left_->cols()[0].tab_name) {
                    lhs_data = left_record_->data + lhs_col->offset;
                } else {
                    lhs_data = right_rec->data + rhs_col->offset - left_->tupleLen();
                }
                if (rhs_col->tab_name == left_->cols()[0].tab_name) {
                    rhs_data = left_record_->data + rhs_col->offset;
                } else {
                    rhs_data = right_rec->data + rhs_col->offset - left_->tupleLen();
                }
                int cmp = 0;
                if (lhs_col->type == TYPE_INT) {
                    cmp = (*(int*)lhs_data > *(int*)rhs_data) - (*(int*)lhs_data < *(int*)rhs_data);
                } else if (lhs_col->type == TYPE_FLOAT) {
                    float lf = *(float*)lhs_data, rf = *(float*)rhs_data;
                    cmp = (lf > rf) - (lf < rf);
                } else {
                    cmp = strncmp(lhs_data, rhs_data, std::min(lhs_col->len, rhs_col->len));
                }
                switch (cond.op) {
                    case OP_EQ: match = (cmp == 0); break;
                    case OP_NE: match = (cmp != 0); break;
                    case OP_LT: match = (cmp < 0); break;
                    case OP_GT: match = (cmp > 0); break;
                    case OP_LE: match = (cmp <= 0); break;
                    case OP_GE: match = (cmp >= 0); break;
                }
                if (!match) break;
            }
            if (!match) return nullptr;
        }
        
        auto join_rec = std::make_unique<RmRecord>(len_);
        memcpy(join_rec->data, left_record_->data, left_->tupleLen());
        memcpy(join_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return join_rec;
    }

private:
    static int compare_col(const char* a, const char* b, ColType type, int len_a, int len_b) {
        if (type == TYPE_INT) {
            int va = *(int*)a, vb = *(int*)b;
            return (va > vb) - (va < vb);
        } else if (type == TYPE_FLOAT) {
            float va = *(float*)a, vb = *(float*)b;
            return (va > vb) - (va < vb);
        } else {
            return strncmp(a, b, std::min(len_a, len_b));
        }
    }

    Rid &rid() override { return _abstract_rid; }
};
