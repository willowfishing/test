/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    SmManager *sm_manager_;
    std::string right_tab_name_;
    std::string right_display_tab_name_;
    std::vector<Condition> right_conds_;
    std::vector<Condition> join_conds_;
    std::vector<std::string> index_col_names_;
    TabCol left_lookup_col_;
    TabCol right_lookup_col_;

    RmFileHandle *right_fh_;
    IndexMeta right_index_meta_;
    std::vector<ColMeta> right_cols_;
    size_t right_len_;
    std::vector<ColMeta> cols_;
    size_t len_;

    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> current_rec_;
    std::unique_ptr<RecScan> scan_;
    Rid rid_;
    bool isend_{false};

    const ColMeta &find_col(const std::vector<ColMeta> &cols, const TabCol &target) const {
        auto it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (it == cols.end()) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
        return *it;
    }

    std::unique_ptr<RmRecord> join_records(const RmRecord *left_rec, const RmRecord *right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_len_);
        return rec;
    }

    void open_lookup_for_current_left() {
        const ColMeta &left_col = find_col(left_->cols(), left_lookup_col_);
        std::vector<char> key(right_index_meta_.col_tot_len);
        memset(key.data(), 0, key.size());
        memcpy(key.data(), left_rec_->data + left_col.offset, right_index_meta_.cols[0].len);
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(right_tab_name_, right_index_meta_.cols)).get();
        Iid lower = ih->lower_bound(key.data());
        Iid upper = ih->upper_bound(key.data());
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
    }

    void advance_to_next_match() {
        current_rec_.reset();
        while (!left_->is_end()) {
            if (scan_ == nullptr) {
                open_lookup_for_current_left();
            }
            while (scan_ != nullptr && !scan_->is_end()) {
                rid_ = scan_->rid();
                scan_->next();
                std::unique_ptr<RmRecord> right_rec;
                try {
                    right_rec = right_fh_->get_record(rid_, context_);
                } catch (RecordNotFoundError &) {
                    continue;
                } catch (PageNotExistError &) {
                    continue;
                }
                if (!eval_conds(right_cols_, right_rec.get(), right_conds_)) {
                    continue;
                }
                auto joined = join_records(left_rec_.get(), right_rec.get());
                if (eval_conds(cols_, joined.get(), join_conds_)) {
                    current_rec_ = std::move(joined);
                    return;
                }
            }
            left_->nextTuple();
            if (left_->is_end()) {
                break;
            }
            left_rec_ = left_->Next();
            scan_.reset();
        }
        isend_ = true;
    }

   public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, SmManager *sm_manager,
                                std::string right_tab_name, std::string right_display_tab_name,
                                std::vector<Condition> right_conds, std::vector<Condition> join_conds,
                                std::vector<std::string> index_col_names, TabCol left_lookup_col,
                                TabCol right_lookup_col, Context *context) {
        left_ = std::move(left);
        sm_manager_ = sm_manager;
        right_tab_name_ = std::move(right_tab_name);
        right_display_tab_name_ = right_display_tab_name.empty() ? right_tab_name_ : std::move(right_display_tab_name);
        right_conds_ = std::move(right_conds);
        join_conds_ = std::move(join_conds);
        index_col_names_ = std::move(index_col_names);
        left_lookup_col_ = std::move(left_lookup_col);
        right_lookup_col_ = std::move(right_lookup_col);
        context_ = context;

        TabMeta &tab = sm_manager_->db_.get_table(right_tab_name_);
        right_index_meta_ = *tab.get_index_meta(index_col_names_);
        right_fh_ = sm_manager_->fhs_.at(right_tab_name_).get();
        right_cols_ = tab.cols;
        for (auto &col : right_cols_) {
            col.tab_name = right_display_tab_name_;
        }
        right_len_ = right_cols_.back().offset + right_cols_.back().len;

        cols_ = left_->cols();
        for (auto col : right_cols_) {
            col.offset += left_->tupleLen();
            cols_.push_back(col);
        }
        len_ = left_->tupleLen() + right_len_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "IndexNestedLoopJoinExecutor"; }

    bool is_end() const override { return isend_; }

    void beginTuple() override {
        left_->beginTuple();
        scan_.reset();
        isend_ = false;
        if (left_->is_end()) {
            isend_ = true;
            return;
        }
        left_rec_ = left_->Next();
        advance_to_next_match();
    }

    void nextTuple() override {
        if (isend_) {
            return;
        }
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_rec_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_rec_);
    }

    Rid &rid() override { return _abstract_rid; }
};