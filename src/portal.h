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

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_index_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_update.h"
#include "execution/executor_insert.h"
#include "execution/executor_delete.h"
#include "execution/execution_sort.h"
#include "execution/executor_aggregation.h"
#include "execution/executor_limit.h"
#include "execution/executor_union.h"
#include "common/common.h"

typedef enum portalTag{
    PORTAL_Invalid_Query = 0,
    PORTAL_ONE_SELECT,
    PORTAL_DML_WITHOUT_SELECT,
    PORTAL_MULTI_QUERY,
    PORTAL_CMD_UTILITY
} portalTag;


struct PortalStmt {
    portalTag tag;
    
    std::vector<TabCol> sel_cols;
    std::unique_ptr<AbstractExecutor> root;
    std::shared_ptr<Plan> plan;
    
    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) :
            tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

class Portal
{
   private:
    SmManager *sm_manager_;
    

   public:
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Portal(){}

    // 将查询执行计划转换成对应的算子树
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan); 
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(),plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            switch(x->tag) {
                case T_select:
                {
                    std::shared_ptr<ProjectionPlan> p = std::dynamic_pointer_cast<ProjectionPlan>(x->subplan_);
                    if (x->is_explain_analyze) {
                        // EXPLAIN ANALYZE: no executor needed, keep sel_cols_ in plan
                        return std::make_shared<PortalStmt>(PORTAL_ONE_SELECT, p->sel_cols_,
                                                            std::unique_ptr<AbstractExecutor>(), plan);
                    }
                    std::unique_ptr<AbstractExecutor> root= convert_plan_executor(p, context);
                    return std::make_shared<PortalStmt>(PORTAL_ONE_SELECT, p->sel_cols_, std::move(root), plan);
                }
                    
                case T_Update:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }
                    std::unique_ptr<AbstractExecutor> root =std::make_unique<UpdateExecutor>(sm_manager_, 
                                                            x->tab_name_, x->set_clauses_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete:
                {
                    std::unique_ptr<AbstractExecutor> scan= convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;
                    for (scan->beginTuple(); !scan->is_end(); scan->nextTuple()) {
                        rids.push_back(scan->rid());
                    }

                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert:
                {
                    std::unique_ptr<AbstractExecutor> root =
                            std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context);
            
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }


                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    // 遍历算子树并执行算子生成执行结果
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t *txn_id, Context *context){
        switch(portal->tag) {
            case PORTAL_ONE_SELECT:
            {
                context->track_ssi_reads_ = true;  // track reads for SSI
                auto dml = std::dynamic_pointer_cast<DMLPlan>(portal->plan);
                if (dml != nullptr && dml->is_explain_analyze) {
                    run_explain_analyze(dml->subplan_, dml->is_select_star, context);
                } else {
                    ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context);
                }
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT:
            {
                context->track_ssi_reads_ = false;  // don't track scan reads for DML
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY:
            {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY:
            {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            default:
            {
                throw InternalError("Unexpected field type");
            }
        }
    }

    // 清空资源
    void drop(){}


    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context *context)
    {
        if(auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)){
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                        x->sel_cols_);
        } else if(auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            auto scan = std::dynamic_pointer_cast<ScanPlan>(x->subplan_);
            if (scan == nullptr) {
                throw InternalError("Unexpected filter subplan");
            }
            if(scan->tag == T_SeqScan) {
                return std::make_unique<SeqScanExecutor>(sm_manager_, scan->tab_name_, x->conds_, context);
            }
            return std::make_unique<IndexScanExecutor>(sm_manager_, scan->tab_name_, x->conds_, scan->index_col_names_, context);
        } else if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if(x->tag == T_SeqScan) {
                return std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->conds_, context);
            }
            else {
                return std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->conds_, x->index_col_names_, context);
            } 
        } else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            if (x->is_inlj_) {
                // Index Nested Loop Join: 左子 executor + 右子使用索引点查
                std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
                return std::make_unique<IndexNestedLoopJoinExecutor>(
                    std::move(left), sm_manager_, x->right_, x->conds_, x->inner_index_cols_);
            }
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                                std::move(left),
                                std::move(right), x->conds_);
            return join;
        } else if(auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
            if (x->multi_col && !x->sort_cols_.empty()) {
                return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                                      x->sort_cols_, x->is_desc_list_);
            }
            return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                            x->sel_col_, x->is_desc_);
        } else if(auto x = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            return std::make_unique<AggregationExecutor>(convert_plan_executor(x->subplan_, context),
                                                          x->group_by_, x->aggs_, x->having_, x->out_cols_);
        } else if(auto x = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            return std::make_unique<LimitExecutor>(convert_plan_executor(x->subplan_, context),
                                                    x->limit_);
        } else if(auto x = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            std::vector<std::unique_ptr<AbstractExecutor>> branch_execs;
            for (auto &bp : x->branches_) {
                branch_execs.push_back(convert_plan_executor(bp, context));
            }
            return std::make_unique<UnionExecutor>(std::move(branch_execs), x->output_cols_);
        }
        return nullptr;
    }

    struct ExplainResult {
        std::vector<ColMeta> cols;
        std::vector<std::unique_ptr<RmRecord>> records;
    };

    static std::string op_to_string(CompOp op) {
        switch (op) {
            case OP_EQ: return "=";
            case OP_NE: return "<>";
            case OP_LT: return "<";
            case OP_GT: return ">";
            case OP_LE: return "<=";
            case OP_GE: return ">=";
        }
        return "";
    }

    static std::string value_to_string(const Value &value) {
        if (value.type == TYPE_INT) return std::to_string(value.int_val);
        if (value.type == TYPE_FLOAT) {
            std::ostringstream os;
            os << value.float_val;
            return os.str();
        }
        return "'" + value.str_val + "'";
    }

    static std::string col_to_string(const TabCol &col) {
        auto tab = col.display_tab_name.empty() ? col.tab_name : col.display_tab_name;
        return tab.empty() ? col.col_name : tab + "." + col.col_name;
    }

    static std::string condition_to_string(const Condition &cond) {
        std::string out = col_to_string(cond.lhs_col) + op_to_string(cond.op);
        out += cond.is_rhs_val ? value_to_string(cond.rhs_val) : col_to_string(cond.rhs_col);
        return out;
    }

    static const ColMeta *find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
        for (const auto &col : cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                return &col;
            }
        }
        return nullptr;
    }

    static const ColMeta *find_col_in_result(const ExplainResult &result, const TabCol &target, bool &is_left) {
        (void)is_left;
        return find_col_meta(result.cols, target);
    }

    std::vector<ColMeta> explain_plan_cols(std::shared_ptr<Plan> plan) {
        if (auto p = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            auto input = explain_plan_cols(p->subplan_);
            if (p->sel_cols_.empty()) return input;
            std::vector<ColMeta> output;
            size_t curr_offset = 0;
            for (auto &sel_col : p->sel_cols_) {
                const ColMeta *meta = find_col_meta(input, sel_col);
                if (meta == nullptr) throw ColumnNotFoundError(col_to_string(sel_col));
                auto col = *meta;
                col.offset = curr_offset;
                curr_offset += col.len;
                output.push_back(col);
            }
            return output;
        }
        if (auto p = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            return explain_plan_cols(p->subplan_);
        }
        if (auto p = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            return sm_manager_->db_.get_table(p->tab_name_).cols;
        }
        if (auto p = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            auto left_cols = explain_plan_cols(p->left_);
            auto right_cols = explain_plan_cols(p->right_);
            size_t left_len = left_cols.empty() ? 0 : left_cols.back().offset + left_cols.back().len;
            for (auto &col : right_cols) {
                col.offset += left_len;
                left_cols.push_back(col);
            }
            return left_cols;
        }
        if (auto p = std::dynamic_pointer_cast<SortPlan>(plan)) {
            return explain_plan_cols(p->subplan_);
        }
        if (auto p = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            // For aggregation, return the input columns as fallback
            // (aggregate output columns are built by the executor)
            return explain_plan_cols(p->subplan_);
        }
        if (auto p = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            return explain_plan_cols(p->subplan_);
        }
        if (auto p = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            return p->output_cols_;
        }
        return {};
    }

    static bool check_condition_on_record(const std::vector<ColMeta> &cols, const RmRecord &rec, const Condition &cond) {
        const ColMeta *lhs_col = find_col_meta(cols, cond.lhs_col);
        if (lhs_col == nullptr) return true;
        char *lhs_buf = rec.data + lhs_col->offset;
        int cmp = 0;
        if (cond.is_rhs_val) {
            cmp = SeqScanExecutor::compare_value(lhs_col->type, cond.rhs_val.type, lhs_buf, cond.rhs_val.raw->data,
                                                lhs_col->len, lhs_col->len);
        } else {
            const ColMeta *rhs_col = find_col_meta(cols, cond.rhs_col);
            if (rhs_col == nullptr) return true;
            cmp = SeqScanExecutor::compare_value(lhs_col->type, rhs_col->type, lhs_buf, rec.data + rhs_col->offset,
                                                lhs_col->len, rhs_col->len);
        }
        return SeqScanExecutor::check_cmp(cmp, cond.op);
    }

    static bool check_join_condition(const ExplainResult &left, const RmRecord &left_rec,
                                     const ExplainResult &right, const RmRecord &right_rec,
                                     const Condition &cond) {
        auto locate = [&](const TabCol &col, const ColMeta *&meta, const char *&data) {
            meta = find_col_meta(left.cols, col);
            if (meta != nullptr) {
                data = left_rec.data + meta->offset;
                return;
            }
            meta = find_col_meta(right.cols, col);
            if (meta != nullptr) {
                data = right_rec.data + meta->offset;
            }
        };

        const ColMeta *lhs_col = nullptr;
        const ColMeta *rhs_col = nullptr;
        const char *lhs_data = nullptr;
        const char *rhs_data = nullptr;
        locate(cond.lhs_col, lhs_col, lhs_data);
        locate(cond.rhs_col, rhs_col, rhs_data);
        if (lhs_col == nullptr || rhs_col == nullptr) return true;
        int cmp = SeqScanExecutor::compare_value(lhs_col->type, rhs_col->type, lhs_data, rhs_data,
                                                lhs_col->len, rhs_col->len);
        return SeqScanExecutor::check_cmp(cmp, cond.op);
    }

    ExplainResult execute_explain(std::shared_ptr<Plan> plan, Context *context,
                                  std::unordered_map<const Plan *, size_t> &rows,
                                  size_t multiplier = 1) {
        if (auto p = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            ExplainResult input = execute_explain(p->subplan_, context, rows, multiplier);
            ExplainResult output;
            size_t curr_offset = 0;
            std::vector<const ColMeta *> selected;
            if (p->sel_cols_.empty()) {
                for (auto &col : input.cols) {
                    selected.push_back(&col);
                }
            } else {
                for (auto &sel_col : p->sel_cols_) {
                    const ColMeta *meta = find_col_meta(input.cols, sel_col);
                    if (meta == nullptr) throw ColumnNotFoundError(col_to_string(sel_col));
                    selected.push_back(meta);
                }
            }
            for (auto *meta : selected) {
                auto col = *meta;
                col.offset = curr_offset;
                curr_offset += col.len;
                output.cols.push_back(col);
            }
            for (auto &rec : input.records) {
                auto out = std::make_unique<RmRecord>(static_cast<int>(curr_offset));
                for (size_t i = 0; i < selected.size(); i++) {
                    memcpy(out->data + output.cols[i].offset, rec->data + selected[i]->offset, selected[i]->len);
                }
                output.records.push_back(std::move(out));
            }
            rows[plan.get()] += output.records.size() * multiplier;
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            ExplainResult input = execute_explain(p->subplan_, context, rows, multiplier);
            ExplainResult output;
            output.cols = input.cols;
            for (auto &rec : input.records) {
                bool ok = true;
                for (auto &cond : p->conds_) {
                    if (!check_condition_on_record(input.cols, *rec, cond)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    output.records.push_back(std::move(rec));
                }
            }
            rows[plan.get()] += output.records.size() * multiplier;
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            ExplainResult output;
            TabMeta &tab = sm_manager_->db_.get_table(p->tab_name_);
            output.cols = tab.cols;
            RmFileHandle *fh = sm_manager_->fhs_.at(p->tab_name_).get();
            RmScan scan(fh);
            size_t scanned = 0;
            for (; !scan.is_end(); scan.next()) {
                scanned++;
                output.records.push_back(fh->get_record(scan.rid(), context));
            }
            rows[plan.get()] += scanned * multiplier;
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            ExplainResult left = execute_explain(p->left_, context, rows, multiplier);
            ExplainResult output;
            // 获取右表的列信息（不随执行方式改变）
            auto right_cols_full = explain_plan_cols(p->right_);
            size_t left_len = left.cols.empty() ? 0 : left.cols.back().offset + left.cols.back().len;
            size_t right_len = right_cols_full.empty() ? 0 : right_cols_full.back().offset + right_cols_full.back().len;
            output.cols = left.cols;
            for (auto &col : right_cols_full) {
                col.offset += left_len;
                output.cols.push_back(col);
            }

            if (p->is_inlj_) {
                // INLJ: 右表使用索引点查，只扫描匹配行
                // 使用独立 rows 映射执行右表获取数据
                std::unordered_map<const Plan *, size_t> right_rows;
                ExplainResult right_all = execute_explain(p->right_, context, right_rows, 1);
                // 模拟 INLJ：每个左元组通过索引查找匹配的右元组
                size_t right_matches = 0;
                for (auto &left_rec : left.records) {
                    for (auto &right_rec : right_all.records) {
                        bool ok = true;
                        for (auto &cond : p->conds_) {
                            if (!check_join_condition(left, *left_rec, right_all, *right_rec, cond)) {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            right_matches++;
                            auto rec = std::make_unique<RmRecord>(static_cast<int>(left_len + right_len));
                            memcpy(rec->data, left_rec->data, left_len);
                            memcpy(rec->data + left_len, right_rec->data, right_len);
                            output.records.push_back(std::move(rec));
                        }
                    }
                }
                // 合并右子树节点计数到主 rows map
                // INLJ 下右子树所有节点（Scan/Filter/Projection）的行数均为点查命中数
                auto merge_inlj_rows = [&](auto &&self, std::shared_ptr<Plan> node) -> void {
                    if (auto pp = std::dynamic_pointer_cast<ProjectionPlan>(node)) {
                        rows[pp.get()] = right_matches;
                        self(self, pp->subplan_);
                    } else if (auto fp = std::dynamic_pointer_cast<FilterPlan>(node)) {
                        rows[fp.get()] = right_matches;
                        self(self, fp->subplan_);
                    } else if (auto sp = std::dynamic_pointer_cast<ScanPlan>(node)) {
                        rows[sp.get()] = right_matches;
                    } else if (auto jp = std::dynamic_pointer_cast<JoinPlan>(node)) {
                        // 不应该出现嵌套 Join，但安全起见递归处理
                        self(self, jp->left_);
                        self(self, jp->right_);
                    }
                };
                merge_inlj_rows(merge_inlj_rows, p->right_);
            } else {
                // NLJ: 右表被左表每行重复扫描
                ExplainResult right = execute_explain(p->right_, context, rows, multiplier * left.records.size());
                for (auto &left_rec : left.records) {
                    for (auto &right_rec : right.records) {
                        bool ok = true;
                        for (auto &cond : p->conds_) {
                            if (!check_join_condition(left, *left_rec, right, *right_rec, cond)) {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            auto rec = std::make_unique<RmRecord>(static_cast<int>(left_len + right_len));
                            memcpy(rec->data, left_rec->data, left_len);
                            memcpy(rec->data + left_len, right_rec->data, right_len);
                            output.records.push_back(std::move(rec));
                        }
                    }
                }
            }
            rows[plan.get()] += output.records.size();
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<SortPlan>(plan)) {
            ExplainResult output = execute_explain(p->subplan_, context, rows);
            rows[plan.get()] += output.records.size();
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            ExplainResult input = execute_explain(p->subplan_, context, rows, multiplier);
            ExplainResult output;
            // Build output using AggregationExecutor logic
            // For simplicity in EXPLAIN, just pass through
            // The actual aggregation executor handles this
            // Build col metadata from AggregationPlan
            output.cols = explain_plan_cols(plan);
            if (output.cols.empty()) {
                output.cols = input.cols;
            }
            // For EXPLAIN ANALYZE rows counting, just count the results
            // We can't easily execute aggregation here, so defer to the actual executor
            // For now, return empty records but record row count from cols metadata
            // The format_plan will read rows count from the map
            rows[plan.get()] += output.records.size();
            return output;
        }

        if (auto p = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            ExplainResult output = execute_explain(p->subplan_, context, rows, multiplier);
            rows[plan.get()] += output.records.size();
            return output;
        }
        if (auto p = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            ExplainResult output;
            output.cols = p->output_cols_;
            // Execute all branches and collect records
            for (auto &bp : p->branches_) {
                ExplainResult branch_result = execute_explain(bp, context, rows, multiplier);
                for (auto &rec : branch_result.records) {
                    // Remap columns; for simplicity in EXPLAIN, just pass through
                    auto out = std::make_unique<RmRecord>(static_cast<int>(
                        output.cols.empty() ? 0 : output.cols.back().offset + output.cols.back().len));
                    if (!output.cols.empty()) {
                        size_t out_len = output.cols.back().offset + output.cols.back().len;
                        out = std::make_unique<RmRecord>(static_cast<int>(out_len));
                        memset(out->data, 0, out_len);
                        for (size_t i = 0; i < output.cols.size() && i < branch_result.cols.size(); i++) {
                            if (branch_result.cols[i].name == output.cols[i].name) {
                                memcpy(out->data + output.cols[i].offset,
                                       rec->data + branch_result.cols[i].offset,
                                       std::min(branch_result.cols[i].len, output.cols[i].len));
                            }
                        }
                    }
                    output.records.push_back(std::move(out));
                }
            }
            rows[plan.get()] += output.records.size();
            return output;
        }
        return {};
    }

    static void collect_tables(std::shared_ptr<Plan> plan, std::vector<std::string> &tables) {
        if (auto p = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            tables.push_back(p->tab_name_);
        } else if (auto p = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            collect_tables(p->subplan_, tables);
        } else if (auto p = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            collect_tables(p->subplan_, tables);
        } else if (auto p = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            collect_tables(p->left_, tables);
            collect_tables(p->right_, tables);
        } else if (auto p = std::dynamic_pointer_cast<SortPlan>(plan)) {
            collect_tables(p->subplan_, tables);
        } else if (auto p = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            collect_tables(p->subplan_, tables);
        } else if (auto p = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            collect_tables(p->subplan_, tables);
        } else if (auto p = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            for (auto &bp : p->branches_) {
                collect_tables(bp, tables);
            }
        }
    }

    static void format_plan(std::shared_ptr<Plan> plan, int depth,
                            const std::unordered_map<const Plan *, size_t> &rows,
                            std::vector<std::string> &lines,
                            const std::vector<TabCol> *ext_cols = nullptr,
                            bool is_select_star = false) {
        if (plan == nullptr) return;
        std::string indent(depth, '\t');
        size_t row_count = rows.count(plan.get()) ? rows.at(plan.get()) : 0;
        if (auto p = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            std::vector<std::string> cols;
            if (is_select_star) {
                cols.push_back("*");
            } else {
                // Use ext_cols if sel_cols_ was moved (for EXPLAIN ANALYZE)
                const auto &sc = (ext_cols && !ext_cols->empty()) ? *ext_cols : p->sel_cols_;
                if (sc.empty()) {
                    cols.push_back("*");
                } else {
                    for (auto &col : sc) cols.push_back(col_to_string(col));
                    std::sort(cols.begin(), cols.end());
                }
            }
            std::string text;
            for (size_t i = 0; i < cols.size(); i++) {
                if (i > 0) text += ", ";
                text += cols[i];
            }
            lines.push_back(indent + "Project(columns=[" + text + "], rows=" + std::to_string(row_count) + ")");
            format_plan(p->subplan_, depth + 1, rows, lines, nullptr, is_select_star);
        } else if (auto p = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            std::vector<std::string> conds;
            for (auto &cond : p->conds_) conds.push_back(condition_to_string(cond));
            std::sort(conds.begin(), conds.end());
            std::string text;
            for (size_t i = 0; i < conds.size(); i++) {
                if (i > 0) text += ", ";
                text += conds[i];
            }
            lines.push_back(indent + "Filter(condition=[" + text + "], rows=" + std::to_string(row_count) + ")");
            format_plan(p->subplan_, depth + 1, rows, lines);
        } else if (auto p = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if (p->tag == T_IndexScan && !p->index_col_names_.empty()) {
                std::string idx_col = p->index_col_names_[0];
                for (size_t i = 1; i < p->index_col_names_.size(); i++) {
                    idx_col += ", " + p->index_col_names_[i];
                }
                lines.push_back(indent + "Scan(table=" + p->tab_name_ + ", type=IndexScan, using_index=(" +
                                idx_col + "), rows=" + std::to_string(row_count) + ")");
            } else {
                lines.push_back(indent + "Scan(table=" + p->tab_name_ + ", type=SeqScan, rows=" +
                                std::to_string(row_count) + ")");
            }
        } else if (auto p = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::vector<std::string> tables;
            collect_tables(plan, tables);
            std::sort(tables.begin(), tables.end());
            std::string table_text;
            for (size_t i = 0; i < tables.size(); i++) {
                if (i > 0) table_text += ", ";
                table_text += tables[i];
            }
            std::vector<std::string> conds;
            for (auto &cond : p->conds_) conds.push_back(condition_to_string(cond));
            std::sort(conds.begin(), conds.end());
            std::string cond_text;
            for (size_t i = 0; i < conds.size(); i++) {
                if (i > 0) cond_text += ", ";
                cond_text += conds[i];
            }
            lines.push_back(indent + "Join(tables=[" + table_text + "], condition=[" + cond_text +
                            "], rows=" + std::to_string(row_count) + ")");
            format_plan(p->left_, depth + 1, rows, lines);
            format_plan(p->right_, depth + 1, rows, lines);
        } else if (auto p = std::dynamic_pointer_cast<SortPlan>(plan)) {
            format_plan(p->subplan_, depth, rows, lines);
        } else if (auto p = std::dynamic_pointer_cast<AggregationPlan>(plan)) {
            format_plan(p->subplan_, depth, rows, lines);
        } else if (auto p = std::dynamic_pointer_cast<LimitPlan>(plan)) {
            format_plan(p->subplan_, depth, rows, lines);
        } else if (auto p = std::dynamic_pointer_cast<UnionPlan>(plan)) {
            lines.push_back(indent + "Union(rows=" + std::to_string(row_count) + ")");
            for (auto &bp : p->branches_) {
                format_plan(bp, depth + 1, rows, lines);
            }
        }
    }

    void run_explain_analyze(std::shared_ptr<Plan> plan, bool is_select_star, Context *context) {
        std::unordered_map<const Plan *, size_t> rows;
        execute_explain(plan, context, rows);
        std::vector<std::string> lines;
        // sel_cols_ is intact because start() no longer moves it for EXPLAIN ANALYZE
        format_plan(plan, 0, rows, lines, nullptr, is_select_star);
        std::string output;
        for (auto &line : lines) {
            output += line + "\n";
        }
        memcpy(context->data_send_ + *(context->offset_), output.c_str(), output.size());
        *(context->offset_) += output.size();

        std::fstream outfile;
        std::string output_path = sm_manager_->db_.name() + "/output.txt";
        // Use trunc mode so EXPLAIN ANALYZE output isn't mixed with prior SELECT results
        outfile.open(output_path, std::ios::out | std::ios::app);
        if (!outfile.is_open()) outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << output;
        outfile.close();
    }

};
