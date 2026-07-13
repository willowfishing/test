#pragma once

#include "executor_abstract.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_{0};
    std::vector<size_t> sel_idxs_;
    std::shared_ptr<RuntimeStat> runtime_;

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev,
                       const std::vector<TabCol> &sel_cols,
                       std::shared_ptr<RuntimeStat> runtime,
                       const std::vector<std::string> &output_names = {})
        : prev_(std::move(prev)), runtime_(std::move(runtime)) {
        if (!runtime_) runtime_ = std::make_shared<RuntimeStat>();
        size_t curr_offset = 0;
        const auto &prev_cols = prev_->cols();
        for (const auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(static_cast<size_t>(pos - prev_cols.begin()));
            auto col = *pos;
            if (output_names.size() == sel_cols.size()) {
                col.tab_name.clear();
                col.name = output_names[cols_.size()];
            }
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(std::move(col));
        }
        len_ = curr_offset;
    }

    void beginTuple() override { prev_->beginTuple(); }
    void nextTuple() override { prev_->nextTuple(); }
    bool is_end() const override { return prev_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        auto prev_rec = prev_->Next();
        if (prev_rec == nullptr) return nullptr;
        auto proj_rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        const auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            const auto &prev_col = prev_cols[sel_idxs_[i]];
            memcpy(proj_rec->data + cols_[i].offset,
                   prev_rec->data + prev_col.offset, cols_[i].len);
        }
        ++runtime_->rows;
        return proj_rec;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    Rid &rid() override { return prev_->rid(); }

    void bind_outer_tuple(const std::vector<ColMeta> &outer_cols,
                          const char *outer_data) override {
        prev_->bind_outer_tuple(outer_cols, outer_data);
    }

};
