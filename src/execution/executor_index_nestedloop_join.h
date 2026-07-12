#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::string right_tab_name_;
    std::string right_visible_name_;
    TabMeta right_tab_;
    RmFileHandle *right_fh_;
    IndexMeta index_meta_;
    std::vector<Condition> fed_conds_;
    std::vector<ColMeta> cols_;
    size_t left_len_;
    size_t right_len_;
    size_t len_;
    SmManager *sm_manager_;
    Context *context_;

    bool isend_ = false;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    std::vector<Rid> matched_rids_;
    size_t cursor_ = 0;

    bool build_key_from_left(const RmRecord *left_rec, char *key) {
        int offset = 0;
        for (auto &index_col : index_meta_.cols) {
            bool found = false;
            for (auto &cond : fed_conds_) {
                if (cond.op != OP_EQ || cond.is_rhs_val) {
                    continue;
                }
                TabCol left_col;
                if (cond.rhs_col.tab_name == right_visible_name_ && cond.rhs_col.col_name == index_col.name) {
                    left_col = cond.lhs_col;
                } else if (cond.lhs_col.tab_name == right_visible_name_ && cond.lhs_col.col_name == index_col.name) {
                    left_col = cond.rhs_col;
                } else {
                    continue;
                }
                auto left_meta = get_col(left_->cols(), left_col);
                memcpy(key + offset, left_rec->data + left_meta->offset, index_col.len);
                found = true;
                break;
            }
            if (!found) {
                return false;
            }
            offset += index_col.len;
        }
        return true;
    }

    std::unique_ptr<RmRecord> make_join_record(const RmRecord *left_rec, const RmRecord *right_rec) const {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_len_);
        memcpy(rec->data + left_len_, right_rec->data, right_len_);
        return rec;
    }

    void load_matches_for_left() {
        matched_rids_.clear();
        cursor_ = 0;
        if (left_->is_end()) {
            return;
        }
        left_rec_ = left_->Next();
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(right_tab_name_, index_meta_.cols)).get();
        std::unique_ptr<char[]> key(new char[index_meta_.col_tot_len]);
        if (build_key_from_left(left_rec_.get(), key.get())) {
            ih->get_value(key.get(), &matched_rids_, context_ == nullptr ? nullptr : context_->txn_);
        }
    }

    bool advance_to_match() {
        while (!left_->is_end()) {
            while (cursor_ < matched_rids_.size()) {
                auto candidate = right_fh_->get_record(matched_rids_[cursor_], context_);
                cursor_++;
                auto joined = make_join_record(left_rec_.get(), candidate.get());
                if (eval_conds(cols_, joined.get(), fed_conds_)) {
                    right_rec_ = std::move(candidate);
                    return true;
                }
            }
            left_->nextTuple();
            load_matches_for_left();
        }
        return false;
    }

   public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, SmManager *sm_manager,
                                const std::string &right_tab_name, std::vector<Condition> conds,
                                std::vector<std::string> index_col_names, Context *context,
                                std::string right_visible_name = "") {
        left_ = std::move(left);
        sm_manager_ = sm_manager;
        context_ = context;
        right_tab_name_ = right_tab_name;
        right_visible_name_ = right_visible_name.empty() ? right_tab_name_ : std::move(right_visible_name);
        right_tab_ = sm_manager_->db_.get_table(right_tab_name_);
        right_fh_ = sm_manager_->fhs_.at(right_tab_name_).get();
        index_meta_ = *(right_tab_.get_index_meta(index_col_names));
        fed_conds_ = std::move(conds);

        left_len_ = left_->tupleLen();
        auto right_cols = right_tab_.cols;
        for (auto &col : right_cols) {
            col.tab_name = right_visible_name_;
        }
        right_len_ = right_cols.back().offset + right_cols.back().len;
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        for (auto &col : right_cols) {
            col.offset += left_len_;
            cols_.push_back(col);
        }
    }

    void beginTuple() override {
        isend_ = false;
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
        if (!advance_to_match()) {
            isend_ = true;
        }
    }

    std::unique_ptr<RmRecord> Next() override { return make_join_record(left_rec_.get(), right_rec_.get()); }
    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "IndexNestedLoopJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
