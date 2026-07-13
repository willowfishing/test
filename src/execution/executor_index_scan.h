/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/mvcc_manager.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::vector<MvccRecord> visible_records_;
    size_t visible_pos_{0};

    SmManager *sm_manager_;

    void write_min_key_part(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(dest, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::lowest();
            memcpy(dest, &v, sizeof(float));
        } else {
            memset(dest, 0, col.len);
        }
    }

    void write_max_key_part(char *dest, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(dest, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::max();
            memcpy(dest, &v, sizeof(float));
        } else {
            memset(dest, 0xff, col.len);
        }
    }

    void advance_to_next_match() {
        if (!visible_records_.empty() || scan_ == nullptr) {
            while (visible_pos_ < visible_records_.size()) {
                rid_ = visible_records_[visible_pos_].rid;
                auto &rec = visible_records_[visible_pos_].record;
                if (eval_conds(cols_, rec.get(), fed_conds_)) {
                    MvccManager::RegisterRecordRead(context_->txn_, tab_name_, rid_);
                    return;
                }
                visible_pos_++;
            }
            return;
        }
        while (scan_ != nullptr && !scan_->is_end()) {
            rid_ = scan_->rid();
            try {
                auto rec = fh_->get_record(rid_, context_);
                if (eval_conds(cols_, rec.get(), fed_conds_)) {
                    return;
                }
            } catch (RecordNotFoundError &) {
                // The index cursor can encounter an entry whose base tuple has been removed.
                // Treat it as invisible and keep scanning.
            } catch (PageNotExistError &) {
                // Same handling when a stale index entry points outside the current table file.
            }
            scan_->next();
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        MvccManager::RegisterPredicateRead(context_->txn_, tab_name_, cols_, fed_conds_);
        visible_records_ = MvccManager::CollectVisibleRecords(sm_manager_, tab_name_, context_->txn_);
        visible_pos_ = 0;
        scan_.reset();
        advance_to_next_match();
        return;

        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        std::vector<char> lower(index_meta_.col_tot_len);
        std::vector<char> upper(index_meta_.col_tot_len);
        int key_offset = 0;
        bool lower_open = false;
        bool upper_open = false;
        bool stopped = false;

        for (auto &idx_col : index_meta_.cols) {
            const Condition *eq = nullptr;
            const Condition *lower_cond = nullptr;
            const Condition *upper_cond = nullptr;
            for (auto &cond : fed_conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.col_name != idx_col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    eq = &cond;
                } else if (cond.op == OP_GT || cond.op == OP_GE) {
                    if (lower_cond == nullptr ||
                        compare_raw_values(idx_col.type, cond.rhs_val.raw->data, lower_cond->rhs_val.raw->data, idx_col.len) > 0) {
                        lower_cond = &cond;
                    }
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    if (upper_cond == nullptr ||
                        compare_raw_values(idx_col.type, cond.rhs_val.raw->data, upper_cond->rhs_val.raw->data, idx_col.len) < 0) {
                        upper_cond = &cond;
                    }
                }
            }

            if (!stopped && eq != nullptr) {
                memcpy(lower.data() + key_offset, eq->rhs_val.raw->data, idx_col.len);
                memcpy(upper.data() + key_offset, eq->rhs_val.raw->data, idx_col.len);
            } else if (!stopped && (lower_cond != nullptr || upper_cond != nullptr)) {
                if (lower_cond != nullptr) {
                    memcpy(lower.data() + key_offset, lower_cond->rhs_val.raw->data, idx_col.len);
                    lower_open = lower_cond->op == OP_GT;
                } else {
                    write_min_key_part(lower.data() + key_offset, idx_col);
                }
                if (upper_cond != nullptr) {
                    memcpy(upper.data() + key_offset, upper_cond->rhs_val.raw->data, idx_col.len);
                    upper_open = upper_cond->op == OP_LT;
                } else {
                    write_max_key_part(upper.data() + key_offset, idx_col);
                }
                stopped = true;
            } else {
                write_min_key_part(lower.data() + key_offset, idx_col);
                write_max_key_part(upper.data() + key_offset, idx_col);
                stopped = true;
            }
            key_offset += idx_col.len;
        }

        Iid lower_iid = lower_open ? ih->upper_bound(lower.data()) : ih->lower_bound(lower.data());
        Iid upper_iid = upper_open ? ih->lower_bound(upper.data()) : ih->upper_bound(upper.data());
        scan_ = std::make_unique<IxScan>(ih, lower_iid, upper_iid, sm_manager_->get_bpm());
        advance_to_next_match();
    }

    void nextTuple() override {
        if (!visible_records_.empty() || scan_ == nullptr) {
            if (visible_pos_ < visible_records_.size()) {
                visible_pos_++;
            }
            advance_to_next_match();
            return;
        }
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        if (!visible_records_.empty() || scan_ == nullptr) {
            return std::make_unique<RmRecord>(*visible_records_[visible_pos_].record);
        }
        return fh_->get_record(rid_, context_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "IndexScanExecutor"; }

    bool is_end() const override {
        if (!visible_records_.empty() || scan_ == nullptr) {
            return visible_pos_ >= visible_records_.size();
        }
        return scan_->is_end();
    }

    Rid &rid() override { return rid_; }
};
