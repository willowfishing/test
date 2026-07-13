/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "parser/ast.h"
#include "system/sm.h"

enum PlanTag {
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
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
    T_ExplainSelect,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SetIsolationSnapshot,
    T_SetIsolationSerializable,
    T_StaticCheckpoint,
    T_SeqScan,
    T_IndexScan,
    T_Filter,
    T_NestLoop,
    T_SortMerge,
    T_Sort,
    T_Union,
    T_Aggregate,
    T_Limit,
    T_Projection,
    T_ShowIndex
};

struct RuntimeStat {
    size_t rows{0};
};

class Plan {
   public:
    PlanTag tag{T_Invalid};
    std::shared_ptr<RuntimeStat> runtime_{std::make_shared<RuntimeStat>()};
    virtual ~Plan() = default;
};

class ScanPlan : public Plan {
   public:
    ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name,
             std::string alias, std::vector<Condition> conds,
             std::vector<std::string> index_col_names,
             std::vector<Condition> lookup_conds = {})
        : tab_name_(std::move(tab_name)), alias_(std::move(alias)),
          conds_(std::move(conds)), fed_conds_(conds_),
          index_col_names_(std::move(index_col_names)),
          lookup_conds_(std::move(lookup_conds)) {
        Plan::tag = tag;
        TabMeta &tab = sm_manager->db_.get_table(tab_name_);
        cols_ = tab.cols;
        const std::string qualifier = alias_.empty() ? tab_name_ : alias_;
        for (auto &col : cols_) col.tab_name = qualifier;
        len_ = cols_.back().offset + cols_.back().len;
    }

    ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name,
             std::vector<Condition> conds,
             std::vector<std::string> index_col_names,
             std::vector<Condition> lookup_conds = {})
        : ScanPlan(tag, sm_manager, std::move(tab_name), "", std::move(conds),
                   std::move(index_col_names), std::move(lookup_conds)) {}

    std::string qualifier() const { return alias_.empty() ? tab_name_ : alias_; }

    std::string tab_name_;       // physical table name
    std::string alias_;          // SQL alias, empty when absent
    std::vector<ColMeta> cols_;  // columns use SQL qualifier
    std::vector<Condition> conds_;
    size_t len_{};
    std::vector<Condition> fed_conds_;
    std::vector<std::string> index_col_names_;
    // Equality predicates whose inner-table column is supplied from the
    // current outer tuple by an Index Nested Loop Join.
    std::vector<Condition> lookup_conds_;
};

class FilterPlan : public Plan {
   public:
    FilterPlan(std::shared_ptr<Plan> subplan, std::vector<Condition> conds)
        : subplan_(std::move(subplan)), conds_(std::move(conds)) {
        Plan::tag = T_Filter;
    }

    std::shared_ptr<Plan> subplan_;
    std::vector<Condition> conds_;
};

class JoinPlan : public Plan {
   public:
    JoinPlan(PlanTag tag, std::shared_ptr<Plan> left,
             std::shared_ptr<Plan> right, std::vector<Condition> conds)
        : left_(std::move(left)), right_(std::move(right)),
          conds_(std::move(conds)), type(INNER_JOIN) {
        Plan::tag = tag;
    }

    std::shared_ptr<Plan> left_;
    std::shared_ptr<Plan> right_;
    std::vector<Condition> conds_;
    JoinType type;
};

class ProjectionPlan : public Plan {
   public:
    ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan,
                   std::vector<TabCol> sel_cols, bool display_all = false,
                   std::vector<TabCol> display_cols = {},
                   std::vector<std::string> output_names = {})
        : subplan_(std::move(subplan)), sel_cols_(std::move(sel_cols)),
          display_cols_(std::move(display_cols)), display_all_(display_all),
          output_names_(std::move(output_names)) {
        Plan::tag = tag;
        if (display_cols_.empty() && !display_all_) display_cols_ = sel_cols_;
    }

    std::shared_ptr<Plan> subplan_;
    // Execution order. The root preserves SELECT-list order.
    std::vector<TabCol> sel_cols_;
    // Explain output order, sorted independently from execution order.
    std::vector<TabCol> display_cols_;
    bool display_all_{false};
    std::vector<std::string> output_names_;
};

class SortPlan : public Plan {
   public:
    SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan,
             std::vector<OrderByInfo> order_bys)
        : subplan_(std::move(subplan)), order_bys_(std::move(order_bys)) {
        Plan::tag = tag;
    }

    std::shared_ptr<Plan> subplan_;
    std::vector<OrderByInfo> order_bys_;
};

class UnionPlan : public Plan {
   public:
    UnionPlan(std::vector<std::shared_ptr<Plan>> branches,
              std::vector<ColMeta> output_cols)
        : branches_(std::move(branches)), output_cols_(std::move(output_cols)) {
        Plan::tag = T_Union;
    }

    std::vector<std::shared_ptr<Plan>> branches_;
    std::vector<ColMeta> output_cols_;
};

class AggregatePlan : public Plan {
   public:
    AggregatePlan(std::shared_ptr<Plan> subplan,
                  std::vector<SelectItemInfo> select_items,
                  std::vector<TabCol> group_cols,
                  std::vector<HavingCondition> having_conds)
        : subplan_(std::move(subplan)), select_items_(std::move(select_items)),
          group_cols_(std::move(group_cols)), having_conds_(std::move(having_conds)) {
        Plan::tag = T_Aggregate;
    }

    std::shared_ptr<Plan> subplan_;
    std::vector<SelectItemInfo> select_items_;
    std::vector<TabCol> group_cols_;
    std::vector<HavingCondition> having_conds_;
};

class LimitPlan : public Plan {
   public:
    LimitPlan(std::shared_ptr<Plan> subplan, size_t limit)
        : subplan_(std::move(subplan)), limit_(limit) {
        Plan::tag = T_Limit;
    }

    std::shared_ptr<Plan> subplan_;
    size_t limit_;
};

class DMLPlan : public Plan {
   public:
    DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::string tab_name,
            std::vector<Value> values, std::vector<Condition> conds,
            std::vector<SetClause> set_clauses)
        : subplan_(std::move(subplan)), tab_name_(std::move(tab_name)),
          values_(std::move(values)), conds_(std::move(conds)),
          set_clauses_(std::move(set_clauses)) {
        Plan::tag = tag;
    }

    std::shared_ptr<Plan> subplan_;
    std::string tab_name_;
    std::vector<Value> values_;
    std::vector<Condition> conds_;
    std::vector<SetClause> set_clauses_;
};

class DDLPlan : public Plan {
   public:
    DDLPlan(PlanTag tag, std::string tab_name,
            std::vector<std::string> col_names, std::vector<ColDef> cols)
        : tab_name_(std::move(tab_name)),
          tab_col_names_(std::move(col_names)), cols_(std::move(cols)) {
        Plan::tag = tag;
    }

    std::string tab_name_;
    std::vector<std::string> tab_col_names_;
    std::vector<ColDef> cols_;
};

class OtherPlan : public Plan {
   public:
    OtherPlan(PlanTag tag, std::string tab_name)
        : tab_name_(std::move(tab_name)) {
        Plan::tag = tag;
    }
    std::string tab_name_;
};

class SetKnobPlan : public Plan {
   public:
    SetKnobPlan(ast::SetKnobType knob_type, bool bool_value)
        : set_knob_type_(knob_type), bool_value_(bool_value) {
        Plan::tag = T_SetKnob;
    }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
};

class plannerInfo {
   public:
    std::shared_ptr<ast::SelectStmt> parse;
    std::vector<Condition> where_conds;
    std::vector<TabCol> sel_cols;
    std::shared_ptr<Plan> plan;
    std::vector<std::shared_ptr<Plan>> table_scan_executors;
    std::vector<SetClause> set_clauses;
    explicit plannerInfo(std::shared_ptr<ast::SelectStmt> parse_)
        : parse(std::move(parse_)) {}
};
