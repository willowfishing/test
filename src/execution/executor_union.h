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

#include <unordered_set>
#include <cstring>
#include "execution_defs.h"
#include "executor_abstract.h"

// Task 6: UNION executor — collects tuples from all branches, deduplicates
class UnionExecutor : public AbstractExecutor {
   private:
    std::vector<std::unique_ptr<AbstractExecutor>> branches_;
    std::vector<ColMeta> cols_;
    size_t len_;
    size_t tuple_num_;
    std::vector<std::unique_ptr<RmRecord>> results_;
    size_t current_idx_;

   public:
    UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> branches,
                  std::vector<ColMeta> output_cols)
        : branches_(std::move(branches)), cols_(std::move(output_cols)) {
        // Compute tuple length from output column metadata
        len_ = 0;
        for (auto &col : cols_) {
            size_t end = col.offset + col.len;
            if (end > len_) len_ = end;
        }
        tuple_num_ = 0;
        current_idx_ = 0;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    void beginTuple() override {
        results_.clear();

        // Hash set for deduplication: hash by raw tuple bytes
        struct TupleHash {
            size_t operator()(const std::vector<char> &data) const {
                size_t h = 0;
                for (size_t i = 0; i < data.size(); i++) {
                    h = h * 31 + (unsigned char)data[i];
                }
                return h;
            }
        };
        std::unordered_set<std::vector<char>, TupleHash> seen;

        // Collect all tuples from all branches
        for (auto &branch : branches_) {
            for (branch->beginTuple(); !branch->is_end(); branch->nextTuple()) {
                auto tuple = branch->Next();
                // Build the output tuple: remap columns according to output_cols_
                auto out = std::make_unique<RmRecord>((int)len_);
                memset(out->data, 0, len_);

                const auto &branch_cols = branch->cols();

                // UNION matches columns by position (not by name)
                // The analyzer already validated column counts match
                size_t ncols = std::min(cols_.size(), branch_cols.size());
                for (size_t i = 0; i < ncols; i++) {
                    size_t branch_off = branch_cols[i].offset;
                    size_t copy_len = std::min((size_t)branch_cols[i].len, (size_t)cols_[i].len);
                    const char *src = tuple->data + branch_off;
                    char *dst = out->data + cols_[i].offset;

                    // Handle type promotion: INT -> FLOAT
                    if (branch_cols[i].type == TYPE_INT && cols_[i].type == TYPE_FLOAT) {
                        int int_val = *(int *)src;
                        float float_val = (float)int_val;
                        memcpy(dst, &float_val, sizeof(float));
                    } else if (branch_cols[i].type == TYPE_STRING && cols_[i].type == TYPE_STRING) {
                        // Copy string with actual length (no padding)
                        size_t str_len = strnlen(src, branch_cols[i].len);
                        memset(dst, 0, cols_[i].len);
                        memcpy(dst, src, std::min(str_len, (size_t)cols_[i].len));
                    } else {
                        memcpy(dst, src, copy_len);
                    }
                }

                // Deduplication: hash by raw bytes
                std::vector<char> key(out->data, out->data + len_);
                if (seen.find(key) == seen.end()) {
                    seen.insert(std::move(key));
                    results_.push_back(std::move(out));
                }
            }
        }

        tuple_num_ = results_.size();
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
            auto rec = std::make_unique<RmRecord>((int)len_);
            memcpy(rec->data, results_[current_idx_]->data, len_);
            return rec;
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
