#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;
    size_t cursor_;

public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
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
        // Build key from record for index deletion
        auto old_rec = fh_->get_record(rid, context_);
        for (auto &index : tab_.indexes) {
            char key[index.col_tot_len];
            int off = 0;
            for (size_t i = 0; i < index.col_num; i++) {
                memcpy(key + off, old_rec->data + index.cols[i].offset, index.cols[i].len);
                off += index.cols[i].len;
            }
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            ih->delete_entry(key, context_->txn_);
        }
        fh_->delete_record(rid, context_);
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
