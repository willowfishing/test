#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;
    bool done_;

public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        rids_ = rids;
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
                char key[index.col_tot_len];
                int off = 0;
                for (size_t i = 0; i < index.col_num; i++) {
                    memcpy(key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                    off += index.cols[i].len;
                }
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                if (sm_manager_->ihs_.count(ix_name)) {
                    auto ih = sm_manager_->ihs_.at(ix_name).get();
                    ih->delete_entry(key, context_->txn_);
                }
            }
            fh_->delete_record(rid, context_);
        }
        done_ = true;
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
