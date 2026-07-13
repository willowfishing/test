#pragma once

#include <unordered_map>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class HashJoinExecutor : public AbstractExecutor {
   private:
    struct JoinKeyPart {
        size_t left_idx = 0;
        ColMeta left_col;
        size_t right_idx = 0;
        ColMeta right_col;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t left_len_ = 0;
    size_t right_len_ = 0;
    size_t len_ = 0;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    std::vector<CompiledCondition> compiled_conds_;
    bool compiled_conds_valid_ = false;
    std::vector<JoinKeyPart> key_parts_;

    std::vector<RmRecord> right_records_;
    std::unordered_map<std::string, std::vector<size_t>> hash_table_;

    bool isend_ = false;
    bool has_current_ = false;
    AbstractExecutor::TupleViewRef left_tuple_;
    const std::vector<size_t> *current_bucket_ = nullptr;
    size_t bucket_pos_ = 0;
    mutable RmRecord current_record_;
    TupleView current_view_;
    std::vector<const char *> current_cells_;

    static bool find_col_idx(const std::vector<ColMeta> &cols, const TabCol &target, size_t *idx, ColMeta *col) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (cols[i].tab_name == target.tab_name && cols[i].name == target.col_name) {
                *idx = i;
                *col = cols[i];
                return true;
            }
        }
        return false;
    }

    void build_key_parts() {
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();
        for (const auto &cond : fed_conds_) {
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                continue;
            }
            size_t left_idx = 0;
            size_t right_idx = 0;
            ColMeta left_col;
            ColMeta right_col;
            bool lhs_left = find_col_idx(left_cols, cond.lhs_col, &left_idx, &left_col);
            bool rhs_right = find_col_idx(right_cols, cond.rhs_col, &right_idx, &right_col);
            if (!lhs_left || !rhs_right) {
                bool rhs_left = find_col_idx(left_cols, cond.rhs_col, &left_idx, &left_col);
                bool lhs_right = find_col_idx(right_cols, cond.lhs_col, &right_idx, &right_col);
                if (!rhs_left || !lhs_right) {
                    continue;
                }
            }
            if (left_col.type != right_col.type || left_col.len != right_col.len) {
                continue;
            }
            key_parts_.push_back({left_idx, left_col, right_idx, right_col});
        }
    }

    std::string build_key_from_left(const TupleView &view) const {
        std::string key;
        size_t key_len = 0;
        for (const auto &part : key_parts_) {
            key_len += static_cast<size_t>(part.left_col.len);
        }
        key.reserve(key_len);
        for (const auto &part : key_parts_) {
            key.append(view.cell_at(part.left_col, part.left_idx), static_cast<size_t>(part.left_col.len));
        }
        return key;
    }

    std::string build_key_from_right_record(const RmRecord &record) const {
        std::string key;
        size_t key_len = 0;
        for (const auto &part : key_parts_) {
            key_len += static_cast<size_t>(part.right_col.len);
        }
        key.reserve(key_len);
        for (const auto &part : key_parts_) {
            key.append(record.data + part.right_col.offset, static_cast<size_t>(part.right_col.len));
        }
        return key;
    }

    void build_right_hash_table() {
        right_records_.clear();
        hash_table_.clear();
        right_->beginTuple();
        const auto &right_cols = right_->cols();
        for (; !right_->is_end(); right_->nextTuple()) {
            auto tuple = right_->ReadTupleView();
            if (!tuple) {
                continue;
            }
            RmRecord record(static_cast<int>(right_len_));
            materialize_tuple_view(*tuple.view, right_cols, &record, right_len_);
            std::string key = build_key_from_right_record(record);
            size_t idx = right_records_.size();
            right_records_.push_back(std::move(record));
            hash_table_[std::move(key)].push_back(idx);
        }
    }

    void load_matches_for_left() {
        current_bucket_ = nullptr;
        bucket_pos_ = 0;
        left_tuple_ = {};
        if (left_->is_end()) {
            return;
        }
        left_tuple_ = left_->ReadTupleView();
        if (!left_tuple_) {
            return;
        }
        std::string key = build_key_from_left(*left_tuple_.view);
        auto it = hash_table_.find(key);
        if (it != hash_table_.end()) {
            current_bucket_ = &it->second;
        }
    }

    bool set_current_from_right_record(const RmRecord &right_record) {
        if (!left_tuple_) {
            return false;
        }
        const auto &left_cols = left_->cols();
        const auto &right_cols = right_->cols();
        current_cells_.resize(left_cols.size() + right_cols.size());
        size_t out_idx = 0;
        for (size_t i = 0; i < left_cols.size(); ++i) {
            current_cells_[out_idx++] = left_tuple_.view->cell_at(left_cols[i], i);
        }
        for (const auto &col : right_cols) {
            current_cells_[out_idx++] = right_record.data + col.offset;
        }
        current_view_.record = nullptr;
        current_view_.cells = &current_cells_;
        return compiled_conds_valid_ ? eval_compiled_conds_view(current_view_, compiled_conds_)
                                     : eval_conds_view(cols_, current_view_, fed_conds_);
    }

    bool advance_to_match() {
        while (!left_->is_end()) {
            while (current_bucket_ != nullptr && bucket_pos_ < current_bucket_->size()) {
                const auto &right_record = right_records_[(*current_bucket_)[bucket_pos_++]];
                if (set_current_from_right_record(right_record)) {
                    has_current_ = true;
                    return true;
                }
            }
            left_->nextTuple();
            load_matches_for_left();
        }
        return false;
    }

   public:
    HashJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                     std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += static_cast<int>(left_len_);
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
        compiled_conds_valid_ = compile_conds(cols_, fed_conds_, &compiled_conds_);
        build_key_parts();
        current_record_.Resize(static_cast<int>(len_));
    }

    void beginTuple() override {
        isend_ = false;
        has_current_ = false;
        build_right_hash_table();
        left_->beginTuple();
        load_matches_for_left();
        if (!advance_to_match()) {
            isend_ = true;
        }
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        has_current_ = false;
        if (!advance_to_match()) {
            isend_ = true;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!has_current_) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        materialize_tuple_view(current_view_, cols_, rec.get(), len_);
        return rec;
    }

    const TupleView *CurrentTupleView() const override { return has_current_ ? &current_view_ : nullptr; }

    const RmRecord *CurrentTuple() const override {
        if (!has_current_) {
            return nullptr;
        }
        materialize_tuple_view(current_view_, cols_, &current_record_, len_);
        return &current_record_;
    }

    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "HashJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
