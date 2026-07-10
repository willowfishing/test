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
    bool need_new_left;

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
        need_new_left = true;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta>& cols() const override { return cols_; }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        need_new_left = true;
        isend = false;
    }

    void nextTuple() override {
        if (need_new_left) {
            left_->nextTuple();
            left_record_ = left_->Next();
            if (left_record_ == nullptr) {
                isend = true;
                return;
            }
            need_new_left = false;
            right_->beginTuple();
        }
        right_->nextTuple();
        if (right_->is_end()) {
            need_new_left = true;
            if (!left_->is_end()) {
                nextTuple();  // recurse to get next left
            } else {
                isend = true;
            }
        }
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        auto left_rec = left_record_ ? std::make_unique<RmRecord>(left_->tupleLen()) : nullptr;
        if (left_rec && left_record_) {
            memcpy(left_rec->data, left_record_->data, left_->tupleLen());
        }
        auto right_rec = right_->Next();
        if (right_rec == nullptr) return nullptr;
        
        // Check join condition
        bool match = true;
        for (auto &cond : fed_conds_) {
            auto lhs_col = get_col(cols_, cond.lhs_col);
            auto rhs_col = get_col(cols_, cond.rhs_col);
            char *lhs_data = nullptr;
            char *rhs_data = nullptr;
            if (lhs_col->tab_name == left_->cols()[0].tab_name) {
                lhs_data = (left_rec ? left_rec->data : left_record_->data) + lhs_col->offset;
            } else {
                lhs_data = right_rec->data + lhs_col->offset - left_->tupleLen();
            }
            if (rhs_col->tab_name == left_->cols()[0].tab_name) {
                rhs_data = (left_rec ? left_rec->data : left_record_->data) + rhs_col->offset;
            } else {
                rhs_data = right_rec->data + rhs_col->offset - left_->tupleLen();
            }
            int cmp = compare_col(lhs_data, rhs_data, lhs_col->type, lhs_col->len, rhs_col->len);
            if (!check_compare(cmp, cond.op)) { match = false; break; }
        }
        
        if (match) {
            auto join_rec = std::make_unique<RmRecord>(len_);
            memcpy(join_rec->data, left_record_->data, left_->tupleLen());
            memcpy(join_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
            return join_rec;
        }
        return nullptr;
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
    
    static bool check_compare(int cmp, CompOp op) {
        switch(op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

    Rid &rid() override { return _abstract_rid; }
};
