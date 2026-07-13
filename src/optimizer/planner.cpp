/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

// 检查表在指定列上是否存在索引（用于 INLJ 判断）
bool has_index_on_column(SmManager *sm_manager, std::string tab_name, std::string col_name,
                          std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        if (index.cols.size() == 1 && index.cols[0].name == col_name) {
            index_col_names.push_back(col_name);
            return true;
        }
    }
    return false;
}

// 递归设置子树中的 ScanPlan 为 IndexScan
void set_index_scan(std::shared_ptr<Plan> plan, const std::vector<std::string> &index_col_names) {
    if (auto sp = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        sp->tag = T_IndexScan;
        sp->index_col_names_ = index_col_names;
        return;
    }
    if (auto fp = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        set_index_scan(fp->subplan_, index_col_names);
        return;
    }
    if (auto pp = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        set_index_scan(pp->subplan_, index_col_names);
        return;
    }
}

// 为 JoinPlan 检测并标记 INLJ（右表有可用索引）
void try_mark_inlj(SmManager *sm, std::shared_ptr<JoinPlan> jp,
                   const std::vector<Condition> &join_conds) {
    if (jp->conds_.empty() && join_conds.empty()) return;
    const auto &conds = jp->conds_.empty() ? join_conds : jp->conds_;
    // 收集右子树的所有表名
    std::vector<std::string> right_tables;
    auto collect = [](auto &&self, std::shared_ptr<Plan> plan, std::vector<std::string> &tabs) -> void {
        if (auto sp = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            tabs.push_back(sp->tab_name_);
        } else if (auto fp = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            self(self, fp->subplan_, tabs);
        } else if (auto pp = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            self(self, pp->subplan_, tabs);
        } else if (auto jp2 = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            self(self, jp2->left_, tabs);
            self(self, jp2->right_, tabs);
        }
    };
    collect(collect, jp->right_, right_tables);
    for (auto &jc : conds) {
        for (auto &rtab : right_tables) {
            std::string inner_col;
            if (jc.lhs_col.tab_name == rtab && !jc.is_rhs_val) {
                inner_col = jc.lhs_col.col_name;
            } else if (!jc.is_rhs_val && jc.rhs_col.tab_name == rtab) {
                inner_col = jc.rhs_col.col_name;
            }
            if (!inner_col.empty()) {
                std::vector<std::string> idx_names;
                if (has_index_on_column(sm, rtab, inner_col, idx_names)) {
                    jp->is_inlj_ = true;
                    jp->inner_index_cols_ = idx_names;
                    set_index_scan(jp->right_, idx_names);
                    return;
                }
            }
        }
    }
}

// 目前的索引匹配规则为：完全匹配索引字段，且全部为单点查询，不会自动调整where条件的顺序
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string>& index_col_names) {
    index_col_names.clear();
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);

    // 按每个索引的列顺序从左到右匹配条件，支持 EQ/GT/LT/GE/LE
    for(auto& index : tab.indexes) {
        std::vector<std::string> matched;
        for(auto& idx_col : index.cols) {
            bool found = false;
            CompOp matched_op = OP_EQ;
            for(auto& cond : curr_conds) {
                if(!cond.is_rhs_val) continue;
                if(cond.lhs_col.tab_name != tab_name) continue;
                if(cond.lhs_col.col_name != idx_col.name) continue;
                matched.push_back(idx_col.name);
                matched_op = cond.op;
                found = true;
                break;
            }
            if(!found) break;           // 前缀中断，此索引不能使用
            if(matched_op != OP_EQ) break;  // 范围条件终止前缀（后续列不参与边界）
        }
        if(!matched.empty()) {
            // 返回完整索引列名
            for(auto& col : index.cols) {
                index_col_names.push_back(col.name);
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        bool match_tab = (tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val);
        bool same_table = (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0);
        // 自连接检测：同一表名但不同别名 → 保留为连接条件
        bool self_join = same_table && !it->is_rhs_val &&
            !it->lhs_col.display_tab_name.empty() &&
            !it->rhs_col.display_tab_name.empty() &&
            it->lhs_col.display_tab_name != it->rhs_col.display_tab_name;
        if (!self_join && (match_tab || same_table)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

std::string first_scan_table(std::shared_ptr<Plan> plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        return x->tab_name_;
    }
    if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        return first_scan_table(x->subplan_);
    }
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return first_scan_table(x->subplan_);
    }
    return "";
}

void add_needed_col(const TabCol &col, const std::string &table, std::vector<TabCol> &needed) {
    if (col.tab_name != table) return;
    if (std::find_if(needed.begin(), needed.end(), [&](const TabCol &c) {
            return c.tab_name == col.tab_name && c.col_name == col.col_name;
        }) == needed.end()) {
        needed.push_back(col);
    }
}

std::shared_ptr<Plan> maybe_push_projection(std::shared_ptr<Plan> plan, const std::string &table,
                                            const std::vector<Condition> &conds,
                                            const std::vector<TabCol> &sel_cols,
                                            bool is_select_star) {
    // SELECT * 不需要投影下推 (所有列都需要)
    if (sel_cols.empty() || is_select_star) {
        return plan;
    }
    std::vector<TabCol> needed;
    for (auto &col : sel_cols) {
        add_needed_col(col, table, needed);
    }
    for (auto &cond : conds) {
        add_needed_col(cond.lhs_col, table, needed);
        if (!cond.is_rhs_val) {
            add_needed_col(cond.rhs_col, table, needed);
        }
    }
    if (needed.empty()) {
        return plan;
    }
    std::sort(needed.begin(), needed.end());
    return std::make_shared<ProjectionPlan>(T_Projection, plan, needed);
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<FilterPlan>(plan))
    {
        return push_conds(cond, x->subplan_);
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto tab_name = first_scan_table(plans[i]);
        if(tab_name.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(tab_name);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    std::vector<Condition> all_conds = query->conds;
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        std::shared_ptr<Plan> scan;
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            scan = std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
        if (query->is_explain_analyze && !curr_conds.empty()) {
            // 独立树结构：条件移入 FilterPlan，ScanPlan 不再持有
            if (auto sp = std::dynamic_pointer_cast<ScanPlan>(scan)) {
                sp->conds_.clear();
                sp->fed_conds_.clear();
            }
            table_scan_executors[i] = std::make_shared<FilterPlan>(scan, curr_conds);
        } else {
            table_scan_executors[i] = scan;
        }
        if (query->is_explain_analyze && tables.size() > 1) {
            table_scan_executors[i] = maybe_push_projection(table_scan_executors[i], tables[i], all_conds, query->cols, query->is_select_star);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }

    // EXPLAIN ANALYZE: 按FROM表顺序构建左深树
    if (query->is_explain_analyze) {
        // 严格按 FROM 顺序构建左深树，不做表重排
        std::vector<Condition> remaining_conds = std::move(query->conds);
        std::shared_ptr<Plan> join_root = table_scan_executors[0];
        std::vector<std::string> joined_tables{tables[0]};

        auto is_joined = [&](const std::string &tab_name) {
            return std::find(joined_tables.begin(), joined_tables.end(), tab_name) != joined_tables.end();
        };

        for (size_t i = 1; i < tables.size(); i++) {
            std::vector<Condition> join_conds;
            auto it = remaining_conds.begin();
            while (it != remaining_conds.end()) {
                bool lhs_joined = is_joined(it->lhs_col.tab_name);
                bool rhs_joined = !it->is_rhs_val && is_joined(it->rhs_col.tab_name);
                bool lhs_new = it->lhs_col.tab_name == tables[i];
                bool rhs_new = !it->is_rhs_val && it->rhs_col.tab_name == tables[i];
                if ((lhs_joined && rhs_new) || (rhs_joined && lhs_new)) {
                    join_conds.push_back(*it);
                    it = remaining_conds.erase(it);
                } else {
                    ++it;
                }
            }
            auto jp = std::make_shared<JoinPlan>(T_NestLoop, std::move(join_root),
                                                  std::move(table_scan_executors[i]), join_conds);
            // 检查右表（内表）是否有可用于 INLJ 的索引
            for (auto &jc : join_conds) {
                std::string inner_col;
                // 判断右表 tables[i] 是连接条件的哪一侧
                if (jc.lhs_col.tab_name == tables[i] && !jc.is_rhs_val) {
                    inner_col = jc.lhs_col.col_name;
                } else if (!jc.is_rhs_val && jc.rhs_col.tab_name == tables[i]) {
                    inner_col = jc.rhs_col.col_name;
                }
                if (!inner_col.empty()) {
                    std::vector<std::string> idx_names;
                    if (has_index_on_column(sm_manager_, tables[i], inner_col, idx_names)) {
                        jp->is_inlj_ = true;
                        jp->inner_index_cols_ = idx_names;
                        // 更新右侧子树中的 ScanPlan 为 IndexScan
                        set_index_scan(jp->right_, idx_names);
                        break;
                    }
                }
            }
            join_root = std::move(jp);
            joined_tables.push_back(tables[i]);
        }

        if (!remaining_conds.empty()) {
            if (auto top_join = std::dynamic_pointer_cast<JoinPlan>(join_root)) {
                top_join->conds_.insert(top_join->conds_.end(), remaining_conds.begin(), remaining_conds.end());
            }
        }
        return join_root;
    }

    // 获取where条件
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if(conds.size() >= 1)
    {
        // 有连接条件

        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            //建立join
            // 判断使用哪种join方式
            if(enable_nestedloop_join && enable_sortmerge_join) {
                // 默认nested loop join
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_nestedloop_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_sortmerge_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(left), std::move(right), join_conds);
            } else {
                // error
                throw RMDBError("No join executor selected!");
            }
            try_mark_inlj(sm_manager_, std::dynamic_pointer_cast<JoinPlan>(table_join_executors), join_conds);

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            } 

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop,
                                                                    std::move(left_need_to_join_executors),
                                                                    std::move(right_need_to_join_executors),
                                                                    join_conds);
                try_mark_inlj(sm_manager_, std::dynamic_pointer_cast<JoinPlan>(temp_join_executors), join_conds);
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(temp_join_executors),
                                                                    std::move(table_join_executors),
                                                                    std::vector<Condition>());
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if(isneedreverse) {
                    // 新表通过 rhs_col 找到 → 放在右侧（内表），条件不需要交换
                    std::vector<Condition> join_conds{*it};
                    table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                                        std::move(right_need_to_join_executors), join_conds);
                    try_mark_inlj(sm_manager_, std::dynamic_pointer_cast<JoinPlan>(table_join_executors), join_conds);
                } else {
                    // 新表通过 lhs_col 找到 → 放在左侧（外表）
                    std::vector<Condition> join_conds{*it};
                    table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left_need_to_join_executors),
                                                                        std::move(table_join_executors), join_conds);
                }
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    //连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_scan_executors[i]), 
                                                    std::move(table_join_executors), std::vector<Condition>());
        }
    }

    if (auto jp = std::dynamic_pointer_cast<JoinPlan>(table_join_executors)) {
    } else {
    }
    return table_join_executors;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!x->has_sort) {
        // 检查 query->has_order_by (Task 5 新路径)
        if (!query->has_order_by) {
            return plan;
        }
        // 使用 query->order_by 构建排序计划
        if (query->order_by.size() == 1) {
            return std::make_shared<SortPlan>(T_Sort, std::move(plan),
                                              query->order_by[0].first, query->order_by[0].second);
        } else {
            std::vector<TabCol> scols;
            std::vector<bool> sdescs;
            for (auto &ob : query->order_by) {
                scols.push_back(ob.first);
                sdescs.push_back(ob.second);
            }
            auto sp = std::make_shared<SortPlan>(T_Sort, std::move(plan), scols[0], sdescs[0]);
            sp->init_multi(std::move(scols), std::move(sdescs));
            return sp;
        }
    }
    // Task 6: UNION 查询使用 query->order_by (已包含正确解析的列引用)
    if (query->is_union && query->has_order_by) {
        if (query->order_by.size() == 1) {
            return std::make_shared<SortPlan>(T_Sort, std::move(plan),
                                              query->order_by[0].first, query->order_by[0].second);
        } else {
            std::vector<TabCol> scols;
            std::vector<bool> sdescs;
            for (auto &ob : query->order_by) {
                scols.push_back(ob.first);
                sdescs.push_back(ob.second);
            }
            auto sp = std::make_shared<SortPlan>(T_Sort, std::move(plan), scols[0], sdescs[0]);
            sp->init_multi(std::move(scols), std::move(sdescs));
            return sp;
        }
    }
    std::vector<std::string> tables = query->tables;
    std::vector<ColMeta> all_cols;
    for (auto &sel_tab_name : tables) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
    // 多列排序
    std::vector<TabCol> sort_cols;
    std::vector<bool> sort_descs;
    for (size_t i = 0; i < x->order->cols.size(); i++) {
        TabCol sel_col;
        for (auto &col : all_cols) {
            if(col.name.compare(x->order->cols[i]->col_name) == 0 )
                sel_col = {.tab_name = col.tab_name, .col_name = col.name};
        }
        sort_cols.push_back(sel_col);
        sort_descs.push_back(x->order->orderby_dirs[i] == ast::OrderBy_DESC);
    }
    if (sort_cols.size() == 1) {
        return std::make_shared<SortPlan>(T_Sort, std::move(plan), sort_cols[0], sort_descs[0]);
    }
    auto sp = std::make_shared<SortPlan>(T_Sort, std::move(plan), sort_cols[0], sort_descs[0]);
    sp->init_multi(std::move(sort_cols), std::move(sort_descs));
    return sp;
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->cols;
    auto aggs = query->aggs;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

    // 如果查询有聚合函数或 GROUP BY，插入 AggregationPlan
    if (query->has_agg || query->has_group_by) {
        // 构建输出列顺序: GROUP BY cols + aggregate cols
        std::vector<TabCol> out_cols;
        if (query->is_select_star) {
            // SELECT * with aggregates not meaningful, but handle gracefully
            for (auto &c : sel_cols) out_cols.push_back(c);
        } else {
            // 基于 query->aggs (已解析列名) 和 sel_cols 构建输出列
            // 先添加 GROUP BY 列
            for (auto &c : query->group_by) {
                out_cols.push_back(c);
            }
            // 再添加聚合列（使用已解析的列名）
            for (auto &a : query->aggs) {
                TabCol tc;
                if (a.agg_type == Query::AggInfo::COUNT_ALL) {
                    tc.tab_name = "";
                    tc.col_name = a.alias.empty() ? "COUNT(*)" : a.alias;
                } else {
                    tc.tab_name = a.col.tab_name;   // 已由 analyzer 解析
                    if (a.alias.empty()) {
                        // 无别名时使用聚合函数名+列名作为唯一列名，避免同一列的多个聚合重名
                        const char* agg_names[] = {"COUNT_ALL", "COUNT", "MAX", "MIN", "SUM", "AVG"};
                        tc.col_name = std::string(agg_names[a.agg_type]) + "(" + a.col.col_name + ")";
                    } else {
                        tc.col_name = a.alias;
                    }
                }
                out_cols.push_back(tc);
            }
        }
        // 更新 sel_cols 为输出列（用于后续的 ProjectionPlan）
        sel_cols = out_cols;
        // 转换 Query::AggInfo 到 AggInfo
        std::vector<AggInfo> agg_infos;
        for (auto &qa : query->aggs) {
            AggInfo ai;
            ai.agg_type = static_cast<AggInfo::AggType>(qa.agg_type);
            ai.col = qa.col;
            ai.alias = qa.alias;
            agg_infos.push_back(ai);
        }
        plannerRoot = std::make_shared<AggregationPlan>(T_Aggregation, std::move(plannerRoot),
                                                         std::move(query->group_by),
                                                         std::move(agg_infos),
                                                         std::move(query->having),
                                                         std::move(out_cols));
    }

    // 添加 LIMIT 节点（如果有）
    if (query->has_limit && query->limit >= 0) {
        plannerRoot = std::make_shared<LimitPlan>(T_Limit, std::move(plannerRoot), query->limit);
    }

    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot),
                                                        std::move(sel_cols));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        // 保存标志 (在 std::move(query) 之前)
        bool is_explain = query->is_explain_analyze;
        bool is_star = query->is_select_star;

        // Task 6: UNION 查询特殊处理
        if (query->is_union) {
            // 为每个 UNION 分支生成子计划
            std::vector<std::shared_ptr<Plan>> branch_plans;
            for (auto &bq : query->union_branches) {
                // 为分支生成物理计划 (scan) 并添加投影
                auto scan_plan = make_one_rel(bq);
                std::shared_ptr<Plan> branch_plan;
                if (bq->is_select_star) {
                    // SELECT *: 不需要额外投影, scan 返回所有列
                    branch_plan = scan_plan;
                } else {
                    // 非 SELECT *: 需要投影到分支的 SELECT 列表
                    branch_plan = std::make_shared<ProjectionPlan>(T_Projection,
                        std::move(scan_plan), bq->cols);
                }
                branch_plans.push_back(branch_plan);
            }

            // 创建 UnionPlan
            auto union_plan = std::make_shared<UnionPlan>(
                std::move(branch_plans), query->union_cols);

            // 应用外层 ORDER BY
            auto sorted = generate_sort_plan(query, std::move(union_plan));

            // 应用外层 LIMIT
            if (query->has_limit && query->limit >= 0) {
                sorted = std::make_shared<LimitPlan>(T_Limit, std::move(sorted), query->limit);
            }

            // 应用投影 (SELECT *)
            auto sel_cols = query->cols;
            auto proj = std::make_shared<ProjectionPlan>(T_Projection, std::move(sorted), std::move(sel_cols));

            auto dml = std::make_shared<DMLPlan>(T_select, proj, std::string(), std::vector<Value>(),
                                                        std::vector<Condition>(), std::vector<SetClause>());
            dml->is_explain_analyze = is_explain;
            dml->is_select_star = is_star;
            plannerRoot = dml;
        } else {
            // 生成select语句的查询执行计划
            std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
            auto dml = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                        std::vector<Condition>(), std::vector<SetClause>());
            dml->is_explain_analyze = is_explain;
            dml->is_select_star = is_star;
            plannerRoot = dml;
        }
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
