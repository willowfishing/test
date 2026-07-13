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

#include <cfloat>
#include <cstdint>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"

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

    SmManager *sm_manager_;

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
        index_meta_ = *(tab_.get_index_meta_prefix(index_col_names_));
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

    bool check_condition(const RmRecord &rec) {
        for (auto &cond : fed_conds_) {
            // 找到 lhs 列（可能在其他 executor 的 cols 中）
            const ColMeta *lhs_col = nullptr;
            for (auto &c : cols_) {
                if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) {
                    lhs_col = &c; break;
                }
            }
            if (!lhs_col) continue;
            char *lhs_buf = rec.data + lhs_col->offset;
            if (cond.is_rhs_val) {
                int cmp = SeqScanExecutor::compare_value(lhs_col->type, cond.rhs_val.type, lhs_buf, cond.rhs_val.raw->data, lhs_col->len, lhs_col->len);
                if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
            } else {
                const ColMeta *rhs_col = nullptr;
                for (auto &c : cols_) { if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) { rhs_col = &c; break; } }
                if (!rhs_col) continue;
                char *rhs_buf = rec.data + rhs_col->offset;
                int cmp = SeqScanExecutor::compare_value(lhs_col->type, rhs_col->type, lhs_buf, rhs_buf, lhs_col->len, rhs_col->len);
                if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
            }
        }
        return true;
    }

    void beginTuple() override {
        int col_tot_len = index_meta_.col_tot_len;
        std::vector<char> left_key_buf(col_tot_len, 0);
        std::vector<char> right_key_buf(col_tot_len, 0);

        // 用各类型的 min/max 填充 key buffer
        {
            int off = 0;
            for (size_t idx = 0; idx < index_meta_.cols.size(); idx++) {
                auto &idx_col = index_meta_.cols[idx];
                if (idx_col.type == TYPE_INT) {
                    int min_val = INT32_MIN, max_val = INT32_MAX;
                    memcpy(left_key_buf.data() + off, &min_val, sizeof(int));
                    memcpy(right_key_buf.data() + off, &max_val, sizeof(int));
                } else if (idx_col.type == TYPE_FLOAT) {
                    float min_val = -FLT_MAX, max_val = FLT_MAX;
                    memcpy(left_key_buf.data() + off, &min_val, sizeof(float));
                    memcpy(right_key_buf.data() + off, &max_val, sizeof(float));
                } else {
                    memset(left_key_buf.data() + off, 0, idx_col.len);
                    memset(right_key_buf.data() + off, 0xFF, idx_col.len);
                }
                off += idx_col.len;
            }
        }

        // 记录每列的合并后操作：{EQ/GT/GE/LT/LE}，以及是否遇到范围条件
        enum ColMatch { NONE, EQ_ONLY, RANGE_LEFT, RANGE_RIGHT, RANGE_BOTH };
        std::vector<ColMatch> col_match(index_meta_.cols.size(), NONE);
        bool has_range_col = false;

        // 按索引列顺序处理，合并同列所有条件
        for (size_t idx = 0; idx < index_meta_.cols.size(); idx++) {
            auto &idx_col = index_meta_.cols[idx];
            int col_offset = 0;
            for (size_t j = 0; j < idx; j++) col_offset += index_meta_.cols[j].len;

            bool has_eq = false;
            const char *eq_val = nullptr;
            const char *left_val = nullptr;
            const char *right_val = nullptr;

            for (auto &cond : fed_conds_) {
                if (!cond.is_rhs_val) continue;
                if (cond.lhs_col.col_name != idx_col.name) continue;
                if (cond.lhs_col.tab_name != idx_col.tab_name) continue;

                switch (cond.op) {
                    case OP_EQ:
                        has_eq = true;
                        eq_val = cond.rhs_val.raw->data;
                        break;
                    case OP_GT: case OP_GE:
                        left_val = cond.rhs_val.raw->data;
                        break;
                    case OP_LT: case OP_LE:
                        right_val = cond.rhs_val.raw->data;
                        break;
                    default: break;
                }
            }

            if (has_eq) {
                // EQ 覆写左右边界为精确值
                memcpy(left_key_buf.data() + col_offset, eq_val, idx_col.len);
                memcpy(right_key_buf.data() + col_offset, eq_val, idx_col.len);
                col_match[idx] = EQ_ONLY;
                // EQ 可以继续下一列
            } else if (left_val || right_val) {
                // 范围条件：使用条件值覆写，注意只覆写对应侧
                if (left_val) memcpy(left_key_buf.data() + col_offset, left_val, idx_col.len);
                if (right_val) memcpy(right_key_buf.data() + col_offset, right_val, idx_col.len);
                col_match[idx] = (left_val && right_val) ? RANGE_BOTH :
                                 (left_val) ? RANGE_LEFT : RANGE_RIGHT;
                has_range_col = true;
                break; // 范围条件终止前缀，后续列不再参与边界
            } else {
                // 此列无条件 → 前缀中断
                break;
            }
        }

        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        IxIndexHandle *ih = nullptr;
        if (sm_manager_->ihs_.count(ix_name)) {
            ih = sm_manager_->ihs_.at(ix_name).get();
        }
        if (ih == nullptr) return;

        // 计算扫描区间：根据最后一列的匹配类型决定边界函数
        Iid lower, upper;
        if (!has_range_col) {
            // 全部 EQ：精确点查
            lower = ih->lower_bound(left_key_buf.data());
            upper = ih->upper_bound(right_key_buf.data());
        } else {
            // 有范围条件：lower 用左 key，upper 用右 key
            lower = ih->lower_bound(left_key_buf.data());
            // 如果最后一列是严格不等，需要精确定界
            // upper_bound 已经返回 >target 的位置，lower_bound 返回 >=target
            upper = ih->upper_bound(right_key_buf.data());
        }
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());

        // Index scans always have range conditions — record predicate for phantom detection
        if (context_ && context_->txn_ && context_->txn_mgr_ &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            context_->txn_->add_predicate_read(tab_name_);
        }
        // 前进到第一个满足所有 WHERE 条件且可见的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = get_visible_rec();
            if (rec && check_condition(*rec)) { record_read(); return; }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = get_visible_rec();
            if (rec && check_condition(*rec)) { record_read(); return; }
            scan_->next();
        }
    }

    bool is_visible() {
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr) return true;
        TupleMeta meta = fh_->get_meta(rid_);
        return context_->txn_mgr_->IsTupleVisible(meta, context_->txn_);
    }

    void record_read() {
        if (context_ && context_->txn_ && context_->txn_mgr_ &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            context_->txn_mgr_->AddToGlobalReadSet(rid_, context_->txn_->get_transaction_id());
            context_->txn_->add_to_read_set(rid_);
        }
    }

    std::unique_ptr<RmRecord> get_visible_rec() {
        if (context_ == nullptr || context_->txn_ == nullptr || context_->txn_mgr_ == nullptr)
            return fh_->get_record(rid_, context_);
        return context_->txn_mgr_->GetVisibleVersion(rid_, fh_, context_->txn_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    bool is_end() const override {
        return scan_ == nullptr || scan_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }
};