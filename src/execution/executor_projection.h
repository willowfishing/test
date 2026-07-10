#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<size_t> sel_idxs_;

public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);
        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }
    void nextTuple() override { prev_->nextTuple(); }
    bool is_end() const override { return prev_->is_end(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta>& cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override {
        auto prev_rec = prev_->Next();
        if (prev_rec == nullptr) return nullptr;
        auto proj_rec = std::make_unique<RmRecord>(len_);
        for (size_t i = 0; i < sel_idxs_.size(); i++) {
            auto &prev_col = prev_->cols()[sel_idxs_[i]];
            memcpy(proj_rec->data + cols_[i].offset, prev_rec->data + prev_col.offset, prev_col.len);
        }
        return proj_rec;
    }

    Rid &rid() override { return _abstract_rid; }
};
