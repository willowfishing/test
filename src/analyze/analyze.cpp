/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    // EXPLAIN ANALYZE: unwrap and set flag
    if (auto ea = std::dynamic_pointer_cast<ast::ExplainAnalyzeStmt>(parse)) {
        query->is_explain_analyze = true;
        parse = ea->query;
    }
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理 EXPLAIN ANALYZE 标志 (可能来自 ExplainAnalyzeStmt 或 SelectStmt 自身)
        if (x->is_explain_analyze) {
            query->is_explain_analyze = true;
        }
        // 构建 别名→表名 映射 (必须在 move(x->tabs) 之前)
        std::map<std::string, std::string> alias_map;
        for (size_t i = 0; i < x->aliases.size() && i < x->tabs.size(); i++) {
            if (!x->aliases[i].empty()) {
                alias_map[x->aliases[i]] = x->tabs[i];
            }
        }

        // Task 6: UNION 检测与处理
        auto &union_map = ast::get_union_map();
        bool has_union = false;
        std::string union_alias;
        for (auto &tab : x->tabs) {
            if (union_map.count(tab)) {
                has_union = true;
                union_alias = tab;
                break;
            }
        }

        if (has_union) {
            query->is_union = true;
            query->union_stmt = union_map[union_alias];

            // 递归分析每个分支
            for (auto &sel : query->union_stmt->selects) {
                auto bq = do_analyze(sel);
                query->union_branches.push_back(bq);
            }

            // 从每个分支获取列元数据（按 SELECT 列表顺序），并进行类型提升
            // 先构建每个分支的输出列元数据（按 SELECT 列表顺序）
            std::vector<std::vector<ColMeta>> branch_cols_list;
            for (auto &bq : query->union_branches) {
                std::vector<ColMeta> bcols;
                if (bq->is_select_star) {
                    // SELECT *: 使用表定义顺序的所有列
                    get_all_cols(bq->tables, bcols);
                } else {
                    // 按 SELECT 列表顺序构建列元数据
                    for (auto &sel_col : bq->cols) {
                        // 查找该列在物理表中的元数据
                        std::vector<ColMeta> tab_cols;
                        get_all_cols(bq->tables, tab_cols);
                        bool found = false;
                        for (auto &tc : tab_cols) {
                            if (tc.name == sel_col.col_name &&
                                (sel_col.tab_name.empty() || tc.tab_name == sel_col.tab_name)) {
                                ColMeta mc = tc;
                                mc.tab_name = union_alias;
                                bcols.push_back(mc);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            throw RMDBError("failure");
                        }
                    }
                }
                branch_cols_list.push_back(bcols);
            }

            // 验证列数一致性
            int num_cols = (int)branch_cols_list[0].size();
            for (size_t bi = 1; bi < branch_cols_list.size(); bi++) {
                if ((int)branch_cols_list[bi].size() != num_cols) {
                    throw RMDBError("failure");
                }
            }

            // 构建类型提升后的输出列
            for (size_t ci = 0; ci < branch_cols_list[0].size(); ci++) {
                ColMeta col = branch_cols_list[0][ci];
                col.tab_name = union_alias;  // 使用 UNION 别名作为表名
                for (size_t bi = 1; bi < branch_cols_list.size(); bi++) {
                    auto &bc = branch_cols_list[bi][ci];
                    if (col.type != bc.type) {
                        // INT + FLOAT => FLOAT
                        if (col.type == TYPE_INT && bc.type == TYPE_FLOAT) {
                            col.type = TYPE_FLOAT;
                            col.len = sizeof(float);
                        } else if (col.type == TYPE_FLOAT && bc.type == TYPE_INT) {
                            // stays FLOAT
                        } else if (col.type == TYPE_STRING && bc.type == TYPE_STRING) {
                            // CHAR(n) + CHAR(m) => CHAR(max(n,m))
                            if (bc.len > col.len) {
                                col.len = bc.len;
                            }
                        } else {
                            // incompatible types (e.g., INT vs CHAR)
                            throw RMDBError("failure");
                        }
                    }
                }
                query->union_cols.push_back(col);
            }
        }

        // 处理表名: 排除 UNION 别名 (它不是物理表)
        {
            auto moved_tabs = std::move(x->tabs);
            for (auto &tab : moved_tabs) {
                if (!union_map.count(tab)) {
                    query->tables.push_back(tab);
                }
            }
        }

        // 获取所有物理表的列元数据 (排除 UNION 别名表)
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        // 合并 UNION 输出列
        for (auto &col : query->union_cols) {
            all_cols.push_back(col);
        }

        // 处理target list: 区分普通列和聚合函数 (Col.agg 区分)
        bool has_any_col = false;
        for (auto &sv_col : x->cols) {
            if (sv_col->agg) {
                // 聚合列
                Query::AggInfo agg_info;
                switch (sv_col->agg->agg_type) {
                    case 0:  // COUNT
                        agg_info.agg_type = sv_col->agg->is_star ?
                            Query::AggInfo::COUNT_ALL : Query::AggInfo::COUNT_COL;
                        break;
                    case 1: agg_info.agg_type = Query::AggInfo::MAX; break;
                    case 2: agg_info.agg_type = Query::AggInfo::MIN; break;
                    case 3: agg_info.agg_type = Query::AggInfo::SUM; break;
                    case 4: agg_info.agg_type = Query::AggInfo::AVG; break;
                }
                if (sv_col->agg->col) {
                    TabCol agg_col = {.tab_name = sv_col->agg->col->tab_name,
                                      .col_name = sv_col->agg->col->col_name};
                    if (!agg_col.tab_name.empty() && alias_map.count(agg_col.tab_name)) {
                        agg_col.display_tab_name = agg_col.tab_name;
                        agg_col.tab_name = alias_map[agg_col.tab_name];
                    }
                    agg_col = check_column(all_cols, agg_col);
                    agg_info.col = agg_col;
                }
                agg_info.alias = sv_col->alias;
                query->aggs.push_back(agg_info);
            } else {
                // 普通列
                TabCol sel_col = {.tab_name = sv_col->tab_name, .col_name = sv_col->col_name};
                if (!sel_col.tab_name.empty() && alias_map.count(sel_col.tab_name)) {
                    sel_col.display_tab_name = sel_col.tab_name;
                    sel_col.tab_name = alias_map[sel_col.tab_name];
                }
                // 如果有 AS 别名，用 display_tab_name 暂存别名
                if (!sv_col->alias.empty()) {
                    sel_col.display_tab_name = sv_col->alias;
                }
                query->cols.push_back(sel_col);
                has_any_col = true;
            }
        }
        // SELECT *: 填充所有列
        if (!has_any_col && query->aggs.empty()) {
            query->is_select_star = true;
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        }
        // 推断普通列的表名
        for (auto &sel_col : query->cols) {
            std::string orig_tab = sel_col.tab_name;
            sel_col = check_column(all_cols, sel_col);
            if (!orig_tab.empty() && orig_tab != sel_col.tab_name) {
                sel_col.display_tab_name = orig_tab;
            }
        }
        query->has_agg = !query->aggs.empty();

        // 处理 WHERE 条件
        // 健壮性检查：WHERE 子句中不能用聚合函数 (col_name 以函数名开头)
        get_clause(x->conds, query->conds);
        for (auto &c : query->conds) {
            const std::string &cn = c.lhs_col.col_name;
            if (cn.rfind("COUNT(", 0) == 0 || cn.rfind("MAX(", 0) == 0 ||
                cn.rfind("MIN(", 0) == 0 || cn.rfind("SUM(", 0) == 0 ||
                cn.rfind("AVG(", 0) == 0) {
                throw RMDBError("failure");
            }
        }
        // 在 check_clause 之前解析条件中的别名
        for (auto &cond : query->conds) {
            if (!cond.lhs_col.tab_name.empty() && alias_map.count(cond.lhs_col.tab_name)) {
                cond.lhs_col.display_tab_name = cond.lhs_col.tab_name;
                cond.lhs_col.tab_name = alias_map[cond.lhs_col.tab_name];
            }
            if (!cond.is_rhs_val && !cond.rhs_col.tab_name.empty() && alias_map.count(cond.rhs_col.tab_name)) {
                cond.rhs_col.display_tab_name = cond.rhs_col.tab_name;
                cond.rhs_col.tab_name = alias_map[cond.rhs_col.tab_name];
            }
        }
        check_clause(query->tables, query->conds);

        // 处理 GROUP BY
        if (x->has_group_by) {
            query->has_group_by = true;
            for (auto &sv_col : x->group_by) {
                TabCol gb_col = {.tab_name = sv_col->tab_name, .col_name = sv_col->col_name};
                if (!gb_col.tab_name.empty() && alias_map.count(gb_col.tab_name)) {
                    gb_col.display_tab_name = gb_col.tab_name;
                    gb_col.tab_name = alias_map[gb_col.tab_name];
                }
                gb_col = check_column(all_cols, gb_col);
                query->group_by.push_back(gb_col);
            }
        }

        // 处理 HAVING
        if (x->has_having) {
            query->has_having = true;
            get_clause(x->having, query->having);
            for (auto &cond : query->having) {
                if (!cond.lhs_col.tab_name.empty() && alias_map.count(cond.lhs_col.tab_name)) {
                    cond.lhs_col.display_tab_name = cond.lhs_col.tab_name;
                    cond.lhs_col.tab_name = alias_map[cond.lhs_col.tab_name];
                }
                if (!cond.is_rhs_val && !cond.rhs_col.tab_name.empty() && alias_map.count(cond.rhs_col.tab_name)) {
                    cond.rhs_col.display_tab_name = cond.rhs_col.tab_name;
                    cond.rhs_col.tab_name = alias_map[cond.rhs_col.tab_name];
                }
            }
            // HAVING 允许聚合函数，跳过 check_clause（列验证在执行时由 AggregationExecutor 处理）
        }

        // 处理 ORDER BY
        // Task 6: 对于 UNION 查询，先验证 ORDER BY 列是否存在于 union 输出列中
        if (query->is_union && x->has_sort && x->order) {
            for (auto &sv_col : x->order->cols) {
                bool found = false;
                for (auto &col : query->union_cols) {
                    if (col.name == sv_col->col_name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw RMDBError("failure");
                }
            }
        }
        if (x->has_sort && x->order) {
            query->has_order_by = true;
            for (size_t i = 0; i < x->order->cols.size(); i++) {
                auto sv_col = x->order->cols[i];
                auto dir = x->order->orderby_dirs[i];
                TabCol ob_col = {.tab_name = sv_col->tab_name, .col_name = sv_col->col_name};
                if (!ob_col.tab_name.empty() && alias_map.count(ob_col.tab_name)) {
                    ob_col.display_tab_name = ob_col.tab_name;
                    ob_col.tab_name = alias_map[ob_col.tab_name];
                }
                ob_col = check_column(all_cols, ob_col);
                bool is_desc = (dir == ast::OrderBy_DESC);
                query->order_by.push_back({ob_col, is_desc});
            }
        }

        // 处理 LIMIT
        if (x->has_limit) {
            query->has_limit = true;
            query->limit = x->limit;
        }

        // 健壮性检查1: SELECT 列表中有 GROUP BY 时，非聚合列必须在 GROUP BY 子句中
        if (query->has_group_by && !query->is_select_star) {
            for (auto &sel_col : query->cols) {
                bool found_in_group_by = false;
                for (auto &gb_col : query->group_by) {
                    if (gb_col.tab_name == sel_col.tab_name && gb_col.col_name == sel_col.col_name) {
                        found_in_group_by = true;
                        break;
                    }
                }
                if (!found_in_group_by) {
                    throw RMDBError("failure");
                }
            }
        }

        // 健壮性检查2: WHERE 子句中不能用聚合函数
        // (聚合函数只在 HAVING 和 SELECT 中允许, WHERE 中的条件由 check_clause 处理, 这里额外检查)

    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        // 处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
        // 处理set子句
        for (auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            set_clause.is_self_ref = sv_set_clause->is_self_ref;
            set_clause.self_ref_col = sv_set_clause->self_ref_col;
            query->set_clauses.push_back(set_clause);
        }

    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        /** TODO: Make sure target column exists */
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            // Allow INT to FLOAT implicit conversion
            if (!((lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) ||
                  (lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT))) {
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
            }
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
