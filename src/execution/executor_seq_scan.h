#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;
    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    SmManager *sm_manager_;
    bool is_end_;

    // Check if current record satisfies all conditions
    bool check_record(RmRecord* rec) {
        for (auto &cond : fed_conds_) {
            if (!cond.is_rhs_val) continue;  // skip column-to-column comparisons for now
            auto it = get_col(cols_, cond.lhs_col);
            char *lhs_data = rec->data + it->offset;
            ColType type = it->type;
            int len = it->len;
            int cmp = 0;
            if (type == TYPE_INT) {
                int lhs = *(int*)lhs_data;
                int rhs = cond.rhs_val.int_val;
                cmp = (lhs > rhs) - (lhs < rhs);
            } else if (type == TYPE_FLOAT) {
                float lhs = *(float*)lhs_data;
                float rhs = cond.rhs_val.float_val;
                cmp = (lhs > rhs) - (lhs < rhs);
            } else if (type == TYPE_STRING) {
                cmp = strncmp(lhs_data, cond.rhs_val.str_val.c_str(),
                              std::min(len, (int)cond.rhs_val.str_val.length()));
            }
            bool ok = false;
            switch (cond.op) {
                case OP_EQ: ok = (cmp == 0); break;
                case OP_NE: ok = (cmp != 0); break;
                case OP_LT: ok = (cmp < 0); break;
                case OP_GT: ok = (cmp > 0); break;
                case OP_LE: ok = (cmp <= 0); break;
                case OP_GE: ok = (cmp >= 0); break;
            }
            if (!ok) return false;
        }
        return true;
    }

public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        fed_conds_ = conds_;
        scan_ = std::make_unique<RmScan>(fh_);
        is_end_ = scan_->is_end();
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        is_end_ = scan_->is_end();
    }

    void nextTuple() override {
        do {
            scan_->next();
            is_end_ = scan_->is_end();
            if (is_end_) return;
            // Check conditions - skip non-matching records
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (check_record(rec.get())) break;
        } while (true);
    }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta>& cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) return nullptr;
        rid_ = scan_->rid();
        auto rec = fh_->get_record(rid_, context_);
        if (!check_record(rec.get())) return nullptr;
        return rec;
    }

    Rid &rid() override { return rid_; }
};
