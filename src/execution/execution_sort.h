#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "executor_abstract.h"
#include "executor_seq_scan.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<OrderByInfo> order_bys_;
    std::vector<ColMeta> cols_;
    std::vector<size_t> key_idxs_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t pos_{0};
    std::shared_ptr<RuntimeStat> runtime_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev,
                 std::vector<OrderByInfo> order_bys,
                 std::shared_ptr<RuntimeStat> runtime)
        : prev_(std::move(prev)), order_bys_(std::move(order_bys)),
          cols_(prev_->cols()), runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        for (const auto &order : order_bys_) {
            auto it = get_col(cols_, order.col);
            key_idxs_.push_back(static_cast<size_t>(it - cols_.begin()));
        }
    }

    void beginTuple() override {
        tuples_.clear();
        pos_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto tuple = prev_->Next();
            if (tuple) tuples_.push_back(std::move(tuple));
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < key_idxs_.size(); ++i) {
                const auto &col = cols_[key_idxs_[i]];
                const int cmp = compare_raw(lhs->data + col.offset, col.type, col.len,
                                            rhs->data + col.offset, col.type, col.len);
                if (cmp == 0) continue;
                return order_bys_[i].is_desc ? cmp > 0 : cmp < 0;
            }
            return false;
        });
    }

    void nextTuple() override {
        if (pos_ < tuples_.size()) ++pos_;
    }

    bool is_end() const override { return pos_ >= tuples_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        ++runtime_->rows;
        return std::make_unique<RmRecord>(*tuples_[pos_]);
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return _abstract_rid; }
};
