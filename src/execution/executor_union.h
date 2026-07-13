#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "executor_abstract.h"

// UNION (without ALL): convert every child tuple to the analyzed common
// schema, then retain only the first occurrence of each complete tuple.
class UnionExecutor : public AbstractExecutor {
   private:
    std::vector<std::unique_ptr<AbstractExecutor>> branches_;
    std::vector<ColMeta> cols_;
    size_t len_{0};
    std::vector<std::unique_ptr<RmRecord>> results_;
    size_t pos_{0};
    std::shared_ptr<RuntimeStat> runtime_;

    std::unique_ptr<RmRecord> convert_tuple(
        const RmRecord &source, const std::vector<ColMeta> &source_cols) const {
        if (source_cols.size() != cols_.size()) {
            throw RMDBError("UNION branch column count changed during execution");
        }
        auto output = std::make_unique<RmRecord>(static_cast<int>(len_));
        std::memset(output->data, 0, len_);
        for (size_t i = 0; i < cols_.size(); ++i) {
            const auto &src = source_cols[i];
            const auto &dst = cols_[i];
            const char *src_data = source.data + src.offset;
            char *dst_data = output->data + dst.offset;

            if (dst.type == TYPE_FLOAT) {
                float value;
                if (src.type == TYPE_INT) {
                    value = static_cast<float>(
                        *reinterpret_cast<const int *>(src_data));
                } else if (src.type == TYPE_FLOAT) {
                    value = *reinterpret_cast<const float *>(src_data);
                } else {
                    throw IncompatibleTypeError(coltype2str(dst.type),
                                                coltype2str(src.type));
                }
                // SQL numeric equality treats -0.0 and +0.0 as equal. A
                // canonical zero also makes byte-based duplicate removal safe.
                if (value == 0.0F) value = 0.0F;
                *reinterpret_cast<float *>(dst_data) = value;
            } else if (dst.type == TYPE_INT) {
                if (src.type != TYPE_INT) {
                    throw IncompatibleTypeError(coltype2str(dst.type),
                                                coltype2str(src.type));
                }
                *reinterpret_cast<int *>(dst_data) =
                    *reinterpret_cast<const int *>(src_data);
            } else {
                if (src.type != TYPE_STRING) {
                    throw IncompatibleTypeError(coltype2str(dst.type),
                                                coltype2str(src.type));
                }
                std::memcpy(dst_data, src_data,
                            std::min(static_cast<size_t>(src.len),
                                     static_cast<size_t>(dst.len)));
            }
        }
        return output;
    }

   public:
    UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> branches,
                  std::vector<ColMeta> output_cols,
                  std::shared_ptr<RuntimeStat> runtime)
        : branches_(std::move(branches)), cols_(std::move(output_cols)),
          runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        for (const auto &col : cols_) {
            len_ = std::max(len_, static_cast<size_t>(col.offset + col.len));
        }
    }

    void beginTuple() override {
        results_.clear();
        pos_ = 0;
        std::unordered_set<std::string> seen;
        for (auto &branch : branches_) {
            const auto &source_cols = branch->cols();
            for (branch->beginTuple(); !branch->is_end(); branch->nextTuple()) {
                auto tuple = branch->Next();
                if (!tuple) continue;
                auto converted = convert_tuple(*tuple, source_cols);
                std::string key(converted->data, len_);
                if (seen.insert(std::move(key)).second) {
                    results_.push_back(std::move(converted));
                }
            }
        }
    }

    void nextTuple() override {
        if (pos_ < results_.size()) ++pos_;
    }

    bool is_end() const override { return pos_ >= results_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        ++runtime_->rows;
        return std::make_unique<RmRecord>(*results_[pos_]);
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return _abstract_rid; }
};
