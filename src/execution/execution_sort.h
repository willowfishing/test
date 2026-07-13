/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;              // output column metadata
    std::vector<TabCol> sort_cols_;         // 多列排序键
    std::vector<bool> is_desc_list_;         // 每列是否降序
    size_t tuple_num_;
    std::vector<size_t> sorted_indices_;
    std::vector<std::unique_ptr<RmRecord>> all_tuples_;
    size_t current_idx_;
    size_t len_;                             // tuple length

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_col, bool is_desc) {
        prev_ = std::move(prev);
        sort_cols_.push_back(sel_col);
        is_desc_list_.push_back(is_desc);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        tuple_num_ = 0;
        current_idx_ = 0;
    }

    // 多列排序构造函数
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sort_cols,
                 std::vector<bool> is_desc_list) {
        prev_ = std::move(prev);
        sort_cols_ = std::move(sort_cols);
        is_desc_list_ = std::move(is_desc_list);
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        tuple_num_ = 0;
        current_idx_ = 0;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    void beginTuple() override {
        // 收集所有元组
        all_tuples_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            all_tuples_.push_back(prev_->Next());
        }
        tuple_num_ = all_tuples_.size();

        // 初始化索引并排序
        sorted_indices_.resize(tuple_num_);
        for (size_t i = 0; i < tuple_num_; i++) {
            sorted_indices_[i] = i;
        }

        // 排序
        std::sort(sorted_indices_.begin(), sorted_indices_.end(),
            [this](size_t a, size_t b) {
                for (size_t k = 0; k < sort_cols_.size(); k++) {
                    // 查找排序列在 cols_ 中的位置
                    const ColMeta *sort_col_meta = nullptr;
                    for (const auto &c : cols_) {
                        if (c.tab_name == sort_cols_[k].tab_name && c.name == sort_cols_[k].col_name) {
                            sort_col_meta = &c;
                            break;
                        }
                    }
                    if (sort_col_meta == nullptr) continue;

                    const char *data_a = all_tuples_[a]->data + sort_col_meta->offset;
                    const char *data_b = all_tuples_[b]->data + sort_col_meta->offset;
                    int cmp = SeqScanExecutor::compare_value(
                        sort_col_meta->type, sort_col_meta->type, data_a, data_b,
                        sort_col_meta->len, sort_col_meta->len);
                    if (cmp != 0) {
                        return is_desc_list_[k] ? (cmp > 0) : (cmp < 0);
                    }
                }
                return false; // 所有排序列相等，保持稳定
            });

        current_idx_ = 0;
    }

    void nextTuple() override {
        current_idx_++;
    }

    bool is_end() const override {
        return current_idx_ >= tuple_num_;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_idx_ < tuple_num_) {
            auto rec = std::make_unique<RmRecord>(len_);
            memcpy(rec->data, all_tuples_[sorted_indices_[current_idx_]]->data, len_);
            return rec;
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
