#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    size_t cursor_;

public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
        cursor_ = 0;
    }

    void beginTuple() override { cursor_ = 0; }
    void nextTuple() override { cursor_++; }
    bool is_end() const override { return cursor_ >= rids_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (cursor_ >= rids_.size()) return nullptr;
        Rid rid = rids_[cursor_];
        auto old_rec = fh_->get_record(rid, context_);
        // Delete old index entries
        for (auto &index : tab_.indexes) {
            char old_key[index.col_tot_len];
            int off = 0;
            for (size_t i = 0; i < index.col_num; i++) {
                memcpy(old_key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                off += index.cols[i].len;
            }
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            ih->delete_entry(old_key, context_->txn_);
        }
        // Build new record with set clauses
        RmRecord new_rec(fh_->get_file_hdr().record_size);
        memcpy(new_rec.data, old_rec->data, fh_->get_file_hdr().record_size);
        for (auto &set_clause : set_clauses_) {
            auto col_it = tab_.get_col(set_clause.lhs.col_name);
            // Convert INT/FLOAT types if needed
            if (col_it->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_INT) {
                set_clause.rhs.set_float((float)set_clause.rhs.int_val);
            } else if (col_it->type == TYPE_INT && set_clause.rhs.type == TYPE_FLOAT) {
                set_clause.rhs.set_int((int)set_clause.rhs.float_val);
            }
            set_clause.rhs.init_raw(col_it->len);
            memcpy(new_rec.data + col_it->offset, set_clause.rhs.raw->data, col_it->len);
        }
        fh_->update_record(rid, new_rec.data, context_);
        // Insert new index entries
        for (auto &index : tab_.indexes) {
            char new_key[index.col_tot_len];
            int off = 0;
            for (size_t i = 0; i < index.col_num; i++) {
                memcpy(new_key + off, new_rec.data + index.cols[i].offset, index.cols[i].len);
                off += index.cols[i].len;
            }
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            ih->insert_entry(new_key, rid, context_->txn_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
