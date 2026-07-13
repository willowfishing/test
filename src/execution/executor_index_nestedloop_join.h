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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_seq_scan.h"
#include "executor_projection.h"
#include "index/ix.h"
#include "optimizer/plan.h"
#include "system/sm.h"

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;       // 左儿子（外表/驱动表）
    std::string inner_tab_name_;                   // 右表（内表）表名
    std::vector<ColMeta> cols_;                    // join 后获得记录的字段
    size_t len_;                                   // join 后每条记录的长度
    std::vector<Condition> fed_conds_;             // join 条件
    std::vector<std::string> inner_index_cols_;    // 内表索引列名
    SmManager *sm_manager_;

    // 内表的元数据
    TabMeta inner_tab_;
    RmFileHandle *inner_fh_;
    IxIndexHandle *inner_ih_;

    // 索引列在左表记录中的偏移量映射
    // 对于每个索引列，记录在左表中的 col 索引
    std::vector<size_t> left_key_offsets_;         // 左表 join key 的偏移
    std::vector<ColType> left_key_types_;          // 左表 join key 的类型
    std::vector<int> left_key_lens_;               // 左表 join key 的长度
    int inner_key_total_len_;                       // 内表索引 key 总长度

    // 右表需要的列（可能经过投影下推限制）
    std::vector<ColMeta> right_needed_cols_;
    // 右表 scan 的全量列
    std::vector<ColMeta> inner_all_cols_;
    // 左表 tuple 长度
    size_t left_len_;

    bool isend_;
    std::unique_ptr<IxScan> ix_scan_;

   public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                                SmManager *sm_manager,
                                std::shared_ptr<Plan> right_plan,
                                std::vector<Condition> conds,
                                std::vector<std::string> index_col_names)
        : left_(std::move(left)),
          fed_conds_(std::move(conds)),
          inner_index_cols_(std::move(index_col_names)),
          sm_manager_(sm_manager),
          isend_(false) {

        // 从右子树中提取扫描信息（穿过可能的 Projection / Filter）
        auto inner_scan = extract_scan(right_plan);
        inner_tab_name_ = inner_scan->tab_name_;
        inner_all_cols_ = inner_scan->cols_;

        // 获取内表元数据和文件句柄
        inner_tab_ = sm_manager_->db_.get_table(inner_tab_name_);
        inner_fh_ = sm_manager_->fhs_.at(inner_tab_name_).get();

        // 获取内表索引句柄
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(inner_tab_name_, inner_index_cols_);
        if (sm_manager_->ihs_.count(ix_name)) {
            inner_ih_ = sm_manager_->ihs_.at(ix_name).get();
        } else {
            inner_ih_ = nullptr;
        }

        // 计算索引 key 总长度
        IndexMeta ix_meta = *(inner_tab_.get_index_meta_prefix(inner_index_cols_));
        inner_key_total_len_ = ix_meta.col_tot_len;

        // 确定右表需要的列（考虑投影下推）
        std::vector<ColMeta> right_output_cols;
        if (auto pp = std::dynamic_pointer_cast<ProjectionPlan>(right_plan)) {
            // 有投影下推：只取投影指定的列
            for (auto &sel_col : pp->sel_cols_) {
                for (auto &col : inner_all_cols_) {
                    if (col.name == sel_col.col_name) {
                        right_output_cols.push_back(col);
                        break;
                    }
                }
            }
        } else {
            // 无投影：取全部列
            right_output_cols = inner_all_cols_;
        }
        right_needed_cols_ = right_output_cols;

        // 构建输出列元数据：左表列 + 右表投影列
        left_len_ = left_->tupleLen();
        cols_ = left_->cols();
        for (auto &col : right_output_cols) {
            col.offset += left_len_;
            cols_.push_back(col);
        }
        len_ = left_len_;
        for (auto &col : right_output_cols) {
            len_ = std::max(len_, (size_t)(col.offset + col.len));
        }

        // 建立索引列与左表列的映射
        // 对于每个索引列，找到在左表 cols 中的偏移和类型
        // join 条件格式为 left_col = right_col (或反之)
        std::vector<Condition> swap_conds = fed_conds_;
        for (auto &cond : swap_conds) {
            // 把 rhs 统一调整到内表侧
            if (cond.lhs_col.tab_name == inner_tab_name_ && !cond.is_rhs_val) {
                std::swap(cond.lhs_col, cond.rhs_col);
            }
        }
        for (auto &idx_col_name : inner_index_cols_) {
            bool found = false;
            for (auto &cond : swap_conds) {
                if (!cond.is_rhs_val && cond.rhs_col.col_name == idx_col_name
                    && cond.rhs_col.tab_name == inner_tab_name_) {
                    // 找到：cond.lhs_col 是左表中的列
                    for (size_t li = 0; li < left_->cols().size(); li++) {
                        auto &lc = left_->cols()[li];
                        if (lc.tab_name == cond.lhs_col.tab_name && lc.name == cond.lhs_col.col_name) {
                            left_key_offsets_.push_back(lc.offset);
                            left_key_types_.push_back(lc.type);
                            left_key_lens_.push_back(lc.len);
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
            if (!found) {
                // fallback: search left cols by matching column name
                for (size_t li = 0; li < left_->cols().size(); li++) {
                    auto &lc = left_->cols()[li];
                    if (lc.name == idx_col_name) {
                        left_key_offsets_.push_back(lc.offset);
                        left_key_types_.push_back(lc.type);
                        left_key_lens_.push_back(lc.len);
                        found = true;
                        break;
                    }
                }
            }
            // 如果还是没找到，使用 offset 0（这通常不应该发生）
            if (!found) {
                left_key_offsets_.push_back(0);
                left_key_types_.push_back(TYPE_INT);
                left_key_lens_.push_back(4);
            }
        }
    }

    // 从右子树中提取 ScanPlan（穿过 Projection / Filter）
    static std::shared_ptr<ScanPlan> extract_scan(std::shared_ptr<Plan> plan) {
        if (auto sp = std::dynamic_pointer_cast<ScanPlan>(plan)) return sp;
        if (auto fp = std::dynamic_pointer_cast<FilterPlan>(plan)) return extract_scan(fp->subplan_);
        if (auto pp = std::dynamic_pointer_cast<ProjectionPlan>(plan)) return extract_scan(pp->subplan_);
        throw InternalError("IndexNestedLoopJoin: right side must contain a ScanPlan");
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    // 基于左表记录和索引构建 key buffer，然后做点查
    bool do_index_lookup(const RmRecord &left_rec) {
        if (inner_ih_ == nullptr) return false;
        // 构建 key buffer
        std::vector<char> key_buf(inner_key_total_len_, 0);
        int offset = 0;
        for (size_t i = 0; i < inner_index_cols_.size(); i++) {
            int col_len = inner_index_cols_.size() == 1
                ? inner_key_total_len_
                : inner_tab_.get_index_meta_prefix(inner_index_cols_)->cols[i].len;
            char *src = left_rec.data + left_key_offsets_[i];
            memcpy(key_buf.data() + offset, src, col_len);
            offset += col_len;
        }
        // 精确点查
        Iid lower = inner_ih_->lower_bound(key_buf.data());
        ix_scan_ = std::make_unique<IxScan>(inner_ih_, lower, inner_ih_->upper_bound(key_buf.data()),
                                            sm_manager_->get_bpm());
        return !ix_scan_->is_end();
    }

    bool check_join_condition(const RmRecord &left_rec, const RmRecord &right_rec) {
        if (fed_conds_.empty()) return true;
        for (auto &cond : fed_conds_) {
            // 找到 lhs 列
            const ColMeta *lhs_col = nullptr;
            char *lhs_buf = nullptr;
            for (auto &c : left_->cols()) {
                if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) {
                    lhs_col = &c; lhs_buf = left_rec.data + c.offset; break;
                }
            }
            if (!lhs_col) {
                for (auto &c : inner_all_cols_) {
                    if (c.tab_name == cond.lhs_col.tab_name && c.name == cond.lhs_col.col_name) {
                        lhs_col = &c; lhs_buf = right_rec.data + c.offset; break;
                    }
                }
            }
            if (!lhs_col) continue;

            // 找到 rhs 列
            const ColMeta *rhs_col = nullptr;
            char *rhs_buf = nullptr;
            for (auto &c : left_->cols()) {
                if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) {
                    rhs_col = &c; rhs_buf = left_rec.data + c.offset; break;
                }
            }
            if (!rhs_col) {
                for (auto &c : inner_all_cols_) {
                    if (c.tab_name == cond.rhs_col.tab_name && c.name == cond.rhs_col.col_name) {
                        rhs_col = &c; rhs_buf = right_rec.data + c.offset; break;
                    }
                }
            }
            if (!rhs_col) continue;

            int cmp = SeqScanExecutor::compare_value(lhs_col->type, rhs_col->type, lhs_buf, rhs_buf,
                                                      lhs_col->len, rhs_col->len);
            if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
        }
        return true;
    }

    void beginTuple() override {
        left_->beginTuple();
        if (!left_->is_end()) {
            auto left_rec = left_->Next();
            if (do_index_lookup(*left_rec)) {
                // 遍历索引结果，寻找满足 join 条件的记录
                while (!ix_scan_->is_end()) {
                    Rid inner_rid = ix_scan_->rid();
                    auto right_rec = inner_fh_->get_record(inner_rid, context_);
                    if (check_join_condition(*left_rec, *right_rec)) {
                        return;  // 找到匹配记录
                    }
                    ix_scan_->next();
                }
            }
            // 当前左元组没有匹配，继续尝试下一个左元组
            advance_left();
        }
    }

    void advance_left() {
        left_->nextTuple();
        while (!left_->is_end()) {
            auto left_rec = left_->Next();
            if (do_index_lookup(*left_rec)) {
                while (!ix_scan_->is_end()) {
                    Rid inner_rid = ix_scan_->rid();
                    auto right_rec = inner_fh_->get_record(inner_rid, context_);
                    if (check_join_condition(*left_rec, *right_rec)) {
                        return;
                    }
                    ix_scan_->next();
                }
            }
            left_->nextTuple();
        }
    }

    void nextTuple() override {
        // 尝试在当前索引扫描中寻找下一个匹配
        if (ix_scan_ && !ix_scan_->is_end()) {
            ix_scan_->next();
            while (!ix_scan_->is_end()) {
                Rid inner_rid = ix_scan_->rid();
                auto left_rec = left_->Next();
                auto right_rec = inner_fh_->get_record(inner_rid, context_);
                if (check_join_condition(*left_rec, *right_rec)) {
                    return;
                }
                ix_scan_->next();
            }
        }
        // 当前左元组的索引用完，前进左元组
        advance_left();
    }

    bool is_end() const override {
        return left_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        auto left_rec = left_->Next();
        Rid inner_rid = ix_scan_->rid();
        auto right_rec = inner_fh_->get_record(inner_rid, context_);

        auto join_rec = std::make_unique<RmRecord>(static_cast<int>(len_));
        // 拷贝左表数据
        memcpy(join_rec->data, left_rec->data, left_len_);
        // 只拷贝右表需要的列
        int right_proj_offset = 0;
        for (auto &col : right_needed_cols_) {
            memcpy(join_rec->data + left_len_ + right_proj_offset,
                   right_rec->data + col.offset, col.len);
            right_proj_offset += col.len;
        }
        return join_rec;
    }

    Rid &rid() override { return _abstract_rid; }
};
