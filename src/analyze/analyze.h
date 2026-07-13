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

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"

class Query{
    public:
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree
    // where条件
    std::vector<Condition> conds;
    // 投影列
    std::vector<TabCol> cols;
    // SELECT-list expressions after name/type resolution.
    std::vector<SelectItemInfo> select_items;
    std::vector<TabCol> group_cols;
    std::vector<HavingCondition> having_conds;
    std::vector<OrderByInfo> order_bys;
    std::vector<std::string> output_names;
    bool has_aggregate{false};
    bool grouped{false};
    int limit{-1};
    // Query qualifiers (alias when present, otherwise physical table name).
    std::vector<std::string> tables;
    // Physical table names plus aliases, in FROM-clause order.
    std::vector<ast::TableRef> table_refs;
    bool select_all{false};
    bool explain_analyze{false};
    // Final SELECT output schema, in SELECT-list order. UNION uses this for
    // branch compatibility checks and positional type conversion.
    std::vector<ColMeta> result_cols;
    bool is_union{false};
    std::string union_alias;
    std::vector<std::shared_ptr<Query>> union_branches;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;

    Query(){}

};

class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    void get_all_cols(const std::vector<ast::TableRef> &table_refs, std::vector<ColMeta> &all_cols);
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    QueryExpr resolve_query_expr(const std::shared_ptr<ast::Expr> &expr,
                                 const std::vector<ColMeta> &all_cols);
    void check_clause(const std::vector<ast::TableRef> &table_refs, std::vector<Condition> &conds);
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
};

