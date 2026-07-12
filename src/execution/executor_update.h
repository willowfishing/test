#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    bool done_;

public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        rids_ = std::move(rids);
        context_ = context;
        done_ = false;
    }

    void beginTuple() override { done_ = false; }
    bool is_end() const override { return done_; }

    std::unique_ptr<RmRecord> Next() override {
        if (done_) return nullptr;
        for (auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            for (auto &index : tab_.indexes) {
                char old_key[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; i++) {
                    memcpy(old_key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (sm_manager_->ihs_.count(ix_name)) {
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    ih->delete_entry(old_key, context_->txn_);
                }
            }
            RmRecord new_rec(fh_->get_file_hdr().record_size);
            memcpy(new_rec.data, old_rec->data, fh_->get_file_hdr().record_size);
            for (auto &sc : set_clauses_) {
                auto col_it = tab_.get_col(sc.lhs.col_name);
                Value rhs = sc.rhs;  // make a copy
                if (col_it->type == TYPE_FLOAT && rhs.type == TYPE_INT)
                    rhs.set_float((float)rhs.int_val);
                else if (col_it->type == TYPE_INT && rhs.type == TYPE_FLOAT)
                    rhs.set_int((int)rhs.float_val);
                rhs.init_raw(col_it->len);
                memcpy(new_rec.data + col_it->offset, rhs.raw->data, col_it->len);
            }
            fh_->update_record(rid, new_rec.data, context_);
            for (auto &index : tab_.indexes) {
                char new_key[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; i++) {
                    memcpy(new_key + off, new_rec.data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (sm_manager_->ihs_.count(ix_name)) {
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    ih->insert_entry(new_key, rid, context_->txn_);
                }
            }
        }
        done_ = true;
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
