#pragma once

#include <set>

#include "executor_abstract.h"

class UnionExecutor : public AbstractExecutor {
   private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_ = 0;

    void copy_cell(RmRecord *dst, const ColMeta &dst_col, const RmRecord *src, const ColMeta &src_col) const {
        char *dst_data = dst->data + dst_col.offset;
        const char *src_data = src->data + src_col.offset;
        if (dst_col.type == src_col.type) {
            memset(dst_data, 0, dst_col.len);
            memcpy(dst_data, src_data, std::min(dst_col.len, src_col.len));
            return;
        }
        if (dst_col.type == TYPE_FLOAT && src_col.type == TYPE_INT) {
            *reinterpret_cast<float *>(dst_data) = static_cast<float>(*reinterpret_cast<const int *>(src_data));
            return;
        }
        throw IncompatibleTypeError(coltype2str(dst_col.type), coltype2str(src_col.type));
    }

   public:
    UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children, std::vector<ColMeta> cols) {
        children_ = std::move(children);
        cols_ = std::move(cols);
        len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
    }

    void beginTuple() override {
        tuples_.clear();
        std::set<std::string> seen;
        for (auto &child : children_) {
            child->beginTuple();
            const auto &src_cols = child->cols();
            if (src_cols.size() != cols_.size()) {
                throw RMDBError("union column count mismatch");
            }
            for (; !child->is_end(); child->nextTuple()) {
                auto src = child->Next();
                auto out = std::make_unique<RmRecord>(len_);
                for (size_t i = 0; i < cols_.size(); ++i) {
                    copy_cell(out.get(), cols_[i], src.get(), src_cols[i]);
                }
                std::string key(out->data, len_);
                if (seen.insert(key).second) {
                    tuples_.push_back(std::move(out));
                }
            }
        }
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, tuples_[cursor_]->data, len_);
        return rec;
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "UnionExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
