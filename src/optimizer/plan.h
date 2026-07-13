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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "parser/ast.h"

#include "parser/parser.h"

typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_ShowIndex,
    T_DescTable,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_SetKnob,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SetTransactionIsolation,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_IndexNestLoop,
    T_SortMerge,    // sort merge join
    T_Sort,
    T_Projection,
    T_Aggregate,
    T_Limit,
    T_SemiJoin,
    T_Union,
    T_Explain
} PlanTag;

// 查询执行计划
class Plan
{
public:
    PlanTag tag;
    virtual ~Plan() = default;
};

class ScanPlan : public Plan
{
    public:
        ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                 std::vector<std::string> index_col_names, std::string visible_name = "")
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            visible_name_ = visible_name.empty() ? tab_name_ : std::move(visible_name);
            conds_ = std::move(conds);
            TabMeta &tab = sm_manager->db_.get_table(tab_name_);
            cols_ = tab.cols;
            for (auto &col : cols_) {
                col.tab_name = visible_name_;
            }
            len_ = cols_.back().offset + cols_.back().len;
            fed_conds_ = conds_;
            index_col_names_ = index_col_names;
        
        }
        ~ScanPlan(){}
        // 以下变量同ScanExecutor中的变量
        std::string tab_name_;                     
        std::string visible_name_;
        std::vector<ColMeta> cols_;                
        std::vector<Condition> conds_;             
        size_t len_;                               
        std::vector<Condition> fed_conds_;
        std::vector<std::string> index_col_names_;
    
};

class JoinPlan : public Plan
{
    public:
        JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds,
                 std::vector<std::string> index_col_names = {})
        {
            Plan::tag = tag;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
            index_col_names_ = std::move(index_col_names);
            type = INNER_JOIN;
        }
        ~JoinPlan(){}
        // 左节点
        std::shared_ptr<Plan> left_;
        // 右节点
        std::shared_ptr<Plan> right_;
        // 连接条件
        std::vector<Condition> conds_;
        std::vector<std::string> index_col_names_;
        // future TODO: 后续可以支持的连接类型
        JoinType type;
};

class SemiJoinPlan : public Plan
{
    public:
        SemiJoinPlan(std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds)
        {
            Plan::tag = T_SemiJoin;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
        }
        ~SemiJoinPlan(){}
        std::shared_ptr<Plan> left_;
        std::shared_ptr<Plan> right_;
        std::vector<Condition> conds_;
};

class ProjectionPlan : public Plan
{
    public:
        ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
        }
        ~ProjectionPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        
};

class SortPlan : public Plan
{
    public:
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, TabCol sel_col, bool is_desc)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_col_ = sel_col;
            is_desc_ = is_desc;
        }
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<std::pair<TabCol, bool>> order_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            order_cols_ = std::move(order_cols);
            if (!order_cols_.empty()) {
                sel_col_ = order_cols_[0].first;
                is_desc_ = order_cols_[0].second;
            } else {
                is_desc_ = false;
            }
        }
        ~SortPlan(){}
        std::shared_ptr<Plan> subplan_;
        TabCol sel_col_;
        bool is_desc_;
        std::vector<std::pair<TabCol, bool>> order_cols_;
        
};

class LimitPlan : public Plan
{
    public:
        LimitPlan(std::shared_ptr<Plan> subplan, int limit)
        {
            Plan::tag = T_Limit;
            subplan_ = std::move(subplan);
            limit_ = limit;
        }
        ~LimitPlan(){}
        std::shared_ptr<Plan> subplan_;
        int limit_;
};

class AggregatePlan : public Plan
{
    public:
        AggregatePlan(std::shared_ptr<Plan> subplan,
                      std::vector<std::shared_ptr<ast::SelectItem>> select_items,
                      std::vector<TabCol> group_cols,
                      std::vector<std::shared_ptr<ast::HavingExpr>> having_conds,
                      std::vector<TabCol> output_cols)
        {
            Plan::tag = T_Aggregate;
            subplan_ = std::move(subplan);
            select_items_ = std::move(select_items);
            group_cols_ = std::move(group_cols);
            having_conds_ = std::move(having_conds);
            output_cols_ = std::move(output_cols);
        }
        ~AggregatePlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<std::shared_ptr<ast::SelectItem>> select_items_;
        std::vector<TabCol> group_cols_;
        std::vector<std::shared_ptr<ast::HavingExpr>> having_conds_;
        std::vector<TabCol> output_cols_;
};

class ExplainPlan : public Plan
{
    public:
        ExplainPlan(std::vector<std::string> lines)
        {
            Plan::tag = T_Explain;
            lines_ = std::move(lines);
        }
        ~ExplainPlan(){}
        std::vector<std::string> lines_;
};

class UnionPlan : public Plan
{
    public:
        UnionPlan(std::vector<std::shared_ptr<Plan>> subplans, std::vector<ColMeta> cols)
        {
            Plan::tag = T_Union;
            subplans_ = std::move(subplans);
            cols_ = std::move(cols);
            len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
        }
        ~UnionPlan(){}
        std::vector<std::shared_ptr<Plan>> subplans_;
        std::vector<ColMeta> cols_;
        size_t len_;
};

// dml语句，包括insert; delete; update; select语句　
class DMLPlan : public Plan
{
    public:
        DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan,std::string tab_name,
                std::vector<Value> values, std::vector<Condition> conds,
                std::vector<SetClause> set_clauses)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            tab_name_ = std::move(tab_name);
            values_ = std::move(values);
            conds_ = std::move(conds);
            set_clauses_ = std::move(set_clauses);
        }
        ~DMLPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::string tab_name_;
        std::vector<Value> values_;
        std::vector<Condition> conds_;
        std::vector<SetClause> set_clauses_;
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan
{
    public:
        DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names, std::vector<ColDef> cols)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            cols_ = std::move(cols);
            tab_col_names_ = std::move(col_names);
        }
        ~DDLPlan(){}
        std::string tab_name_;
        std::vector<std::string> tab_col_names_;
        std::vector<ColDef> cols_;
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan
{
    public:
        OtherPlan(PlanTag tag, std::string tab_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);            
        }
        ~OtherPlan(){}
        std::string tab_name_;
};

// Set Knob Plan
class SetKnobPlan : public Plan
{
    public:
        SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
            Plan::tag = T_SetKnob;
            set_knob_type_ = knob_type;
            bool_value_ = bool_value;
        }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
};

class plannerInfo{
    public:
    std::shared_ptr<ast::SelectStmt> parse;
    std::vector<Condition> where_conds;
    std::vector<TabCol> sel_cols;
    std::shared_ptr<Plan> plan;
    std::vector<std::shared_ptr<Plan>> table_scan_executors;
    std::vector<SetClause> set_clauses;
    plannerInfo(std::shared_ptr<ast::SelectStmt> parse_):parse(std::move(parse_)){}

};
