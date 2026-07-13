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

#include <algorithm>
#include <map>
#include <set>

namespace {

std::string real_table_name(const std::string &display,
                            const std::unordered_map<std::string, std::string> *display_to_real) {
    if (display_to_real == nullptr) {
        return display;
    }
    auto it = display_to_real->find(display);
    return it == display_to_real->end() ? display : it->second;
}

bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

bool expr_has_agg(const std::shared_ptr<ast::Expr> &expr) {
    return std::dynamic_pointer_cast<ast::AggFunc>(expr) != nullptr;
}

bool grouped_or_aggregated(const AggExpr &expr, const std::vector<TabCol> &group_cols) {
    if (expr.is_agg()) {
        return true;
    }
    return std::any_of(group_cols.begin(), group_cols.end(),
                       [&](const TabCol &group_col) { return same_col(group_col, expr.col); });
}

}  // namespace

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    std::shared_ptr<ast::TreeNode> semantic_root = parse;

    if (auto explain = std::dynamic_pointer_cast<ast::ExplainAnalyze>(parse)) {
        query->is_explain_analyze = true;
        semantic_root = explain->select;
    }

    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(semantic_root)) {
        if (x->table_refs.empty()) {
            for (auto &tab_name : x->tabs) {
                x->table_refs.push_back(std::make_shared<ast::TableRef>(tab_name));
            }
        }
        query->preserve_join_order = x->has_explicit_join;

        std::set<std::string> display_seen;
        for (auto &ref : x->table_refs) {
            const std::string real = ref->tab_name;
            const std::string display = ref->display_name();
            if (!sm_manager_->db_.is_table(real)) {
                throw TableNotFoundError(real);
            }
            if (!display_seen.insert(display).second) {
                throw RMDBError("Duplicate table name or alias: " + display);
            }
            query->tables.push_back(real);
            query->real_tables.push_back(real);
            query->display_tables.push_back(display);
            query->display_to_real[display] = real;
            query->real_to_display.emplace(real, display);
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->display_tables, all_cols, &query->display_to_real);

        query->select_all = x->select_items.empty() && x->cols.empty();
        if (!x->select_items.empty()) {
            for (auto &sv_item : x->select_items) {
                SelectItem item = convert_sv_agg_expr(sv_item->expr, all_cols);
                item.alias = sv_item->alias;
                query->has_agg = query->has_agg || item.is_agg();
                query->select_items.push_back(item);
                if (!item.is_agg()) {
                    query->cols.push_back(item.col);
                }
            }
        } else {
            for (auto &sv_sel_col : x->cols) {
                TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
                sel_col = check_column(all_cols, sel_col);
                query->cols.push_back(sel_col);
                query->select_items.push_back(SelectItem{.agg_type = AGG_NONE, .col = sel_col});
            }
        }

        for (auto &sv_group_col : x->group_by_cols) {
            TabCol group_col = {.tab_name = sv_group_col->tab_name, .col_name = sv_group_col->col_name};
            query->group_by_cols.push_back(check_column(all_cols, group_col));
        }
        query->has_group_by = !query->group_by_cols.empty();

        for (auto &order : x->orders) {
            TabCol order_col = {.tab_name = order->cols->tab_name, .col_name = order->cols->col_name};
            bool resolved_output_col = false;
            if ((query->has_agg || query->has_group_by) && order_col.tab_name.empty()) {
                for (auto &item : query->select_items) {
                    if (item.display_name() == order_col.col_name) {
                        query->order_by_cols.push_back(TabCol{.tab_name = "", .col_name = order_col.col_name});
                        resolved_output_col = true;
                        break;
                    }
                }
            }
            if (!resolved_output_col) {
                query->order_by_cols.push_back(check_column(all_cols, order_col));
            }
            query->order_by_desc.push_back(order->orderby_dir == ast::OrderBy_DESC);
        }
        query->limit = x->limit;

        get_having_clause(x->having_conds, all_cols, query->having_conds);
        query->has_agg = query->has_agg || !query->having_conds.empty();

        get_clause(x->conds, query->conds);
        check_clause(query->display_tables, query->conds, &query->display_to_real);

        if ((query->has_agg || query->has_group_by) && !query->select_all) {
            for (auto &item : query->select_items) {
                if (!grouped_or_aggregated(item, query->group_by_cols)) {
                    throw RMDBError("SELECT list contains nonaggregated column not in GROUP BY");
                }
            }
            for (auto &cond : query->having_conds) {
                if (!grouped_or_aggregated(cond.lhs, query->group_by_cols) ||
                    (!cond.is_rhs_val && !grouped_or_aggregated(cond.rhs_expr, query->group_by_cols))) {
                    throw RMDBError("HAVING clause contains nonaggregated column not in GROUP BY");
                }
            }
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(semantic_root)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &sv_set_clause : x->set_clauses) {
            auto col_meta = tab.get_col(sv_set_clause->col_name);
            if (sv_set_clause->rhs_op != 0) {
                auto rhs_col_meta = tab.get_col(sv_set_clause->rhs_col_name);
                if (rhs_col_meta->type != TYPE_INT && rhs_col_meta->type != TYPE_FLOAT) {
                    throw IncompatibleTypeError("NUMERIC", coltype2str(rhs_col_meta->type));
                }
                if (col_meta->type != rhs_col_meta->type) {
                    throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(rhs_col_meta->type));
                }
                Value delta = convert_sv_value(sv_set_clause->rhs_delta);
                if (rhs_col_meta->type == TYPE_FLOAT && delta.type == TYPE_INT) {
                    delta.set_float(static_cast<float>(delta.int_val));
                }
                delta.init_raw(rhs_col_meta->len);
                if (rhs_col_meta->type != delta.type) {
                    throw IncompatibleTypeError(coltype2str(rhs_col_meta->type), coltype2str(delta.type));
                }
                query->set_clauses.push_back(SetClause{.lhs = TabCol{.tab_name = x->tab_name, .col_name = sv_set_clause->col_name},
                                                       .rhs = Value(),
                                                       .rhs_is_col_expr = true,
                                                       .rhs_col = TabCol{.tab_name = x->tab_name,
                                                                         .col_name = sv_set_clause->rhs_col_name},
                                                       .rhs_op = sv_set_clause->rhs_op,
                                                       .rhs_delta = delta});
            } else {
                Value value = convert_sv_value(sv_set_clause->val);
                if (col_meta->type == TYPE_FLOAT && value.type == TYPE_INT) {
                    value.set_float(static_cast<float>(value.int_val));
                }
                value.init_raw(col_meta->len);
                if (col_meta->type != value.type) {
                    throw IncompatibleTypeError(coltype2str(col_meta->type), coltype2str(value.type));
                }
                query->set_clauses.push_back(
                    SetClause{.lhs = TabCol{.tab_name = x->tab_name, .col_name = sv_set_clause->col_name}, .rhs = value});
            }
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(semantic_root)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(semantic_root)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    }

    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
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
        return target;
    }

    for (auto &col : all_cols) {
        if (col.tab_name == target.tab_name && col.name == target.col_name) {
            return target;
        }
    }
    throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols,
                           const std::unordered_map<std::string, std::string> *display_to_real) {
    for (auto &tab_name : tab_names) {
        const auto real = real_table_name(tab_name, display_to_real);
        const auto &sel_tab_cols = sm_manager_->db_.get_table(real).cols;
        for (auto col : sel_tab_cols) {
            col.tab_name = tab_name;
            all_cols.push_back(col);
        }
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        auto lhs_col = std::dynamic_pointer_cast<ast::Col>(expr->lhs);
        if (lhs_col == nullptr || expr_has_agg(expr->rhs)) {
            throw RMDBError("Aggregate functions are not allowed in WHERE clause");
        }
        Condition cond;
        cond.lhs_col = {.tab_name = lhs_col->tab_name, .col_name = lhs_col->col_name};
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

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds,
                           const std::unordered_map<std::string, std::string> *display_to_real) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols, display_to_real);

    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }

        TabMeta &lhs_tab = sm_manager_->db_.get_table(real_table_name(cond.lhs_col.tab_name, display_to_real));
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            }
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(real_table_name(cond.rhs_col.tab_name, display_to_real));
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
        val.raw_text = std::to_string(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
        val.raw_text = float_lit->raw_text.empty() ? std::to_string(float_lit->val) : float_lit->raw_text;
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

AggExpr Analyze::convert_sv_agg_expr(const std::shared_ptr<ast::Expr> &expr, const std::vector<ColMeta> &all_cols) {
    AggExpr item;
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr)) {
        item.agg_type = AGG_NONE;
        item.col = check_column(all_cols, TabCol{.tab_name = col->tab_name, .col_name = col->col_name});
        return item;
    }

    if (auto agg = std::dynamic_pointer_cast<ast::AggFunc>(expr)) {
        item.agg_type = agg->agg_type;
        item.count_star = agg->count_star;
        if (!agg->count_star) {
            item.col = check_column(all_cols, TabCol{.tab_name = agg->col->tab_name, .col_name = agg->col->col_name});
            auto col_meta = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == item.col.tab_name && col.name == item.col.col_name;
            });
            if (col_meta == all_cols.end()) {
                throw ColumnNotFoundError(item.col.col_name);
            }
            if (item.agg_type != AGG_COUNT && col_meta->type == TYPE_STRING) {
                throw IncompatibleTypeError("NUMERIC", coltype2str(col_meta->type));
            }
        }
        return item;
    }

    throw RMDBError("Unsupported select expression");
}

void Analyze::get_having_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                                const std::vector<ColMeta> &all_cols, std::vector<HavingCondition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        HavingCondition cond;
        cond.lhs = convert_sv_agg_expr(expr->lhs, all_cols);
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else {
            cond.is_rhs_val = false;
            cond.rhs_expr = convert_sv_agg_expr(expr->rhs, all_cols);
        }
        conds.push_back(cond);
    }
}
