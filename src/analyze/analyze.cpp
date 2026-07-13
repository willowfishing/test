/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "analyze.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace {

const ColMeta &find_col_meta(const std::vector<ColMeta> &all_cols, const TabCol &target) {
    auto it = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (it == all_cols.end()) {
        throw ColumnNotFoundError(target.tab_name.empty() ? target.col_name
                                                          : target.tab_name + "." + target.col_name);
    }
    return *it;
}

bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

bool same_expr(const QueryExpr &lhs, const QueryExpr &rhs) {
    return lhs.is_aggregate == rhs.is_aggregate &&
           lhs.agg_type == rhs.agg_type &&
           lhs.aggregate_star == rhs.aggregate_star &&
           (lhs.aggregate_star || same_col(lhs.col, rhs.col));
}

const char *agg_name(AggFuncType type) {
    switch (type) {
        case AggFuncType::COUNT: return "COUNT";
        case AggFuncType::MAX: return "MAX";
        case AggFuncType::MIN: return "MIN";
        case AggFuncType::SUM: return "SUM";
        case AggFuncType::AVG: return "AVG";
        default: return "";
    }
}

std::string default_output_name(const QueryExpr &expr) {
    if (!expr.is_aggregate) return expr.col.col_name;
    return std::string(agg_name(expr.agg_type)) + "(" +
           (expr.aggregate_star ? "*" : expr.col.col_name) + ")";
}

ColType expr_type(const QueryExpr &expr, const std::vector<ColMeta> &all_cols) {
    if (expr.is_aggregate && expr.agg_type == AggFuncType::COUNT) return TYPE_INT;
    if (expr.is_aggregate && expr.agg_type == AggFuncType::AVG) return TYPE_FLOAT;
    return find_col_meta(all_cols, expr.col).type;
}

std::string ast_value_to_display(const std::shared_ptr<ast::Value> &value) {
    if (auto x = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        if (!x->display.empty()) return x->display;
        return std::to_string(x->val);
    }
    if (auto x = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        if (!x->display.empty()) return x->display;
        std::ostringstream out;
        out << std::defaultfloat << x->val;
        std::string text = out.str();
        if (text.find('.') == std::string::npos &&
            text.find('e') == std::string::npos &&
            text.find('E') == std::string::npos) {
            text += ".0";
        }
        return text;
    }
    if (auto x = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        std::string escaped;
        for (char ch : x->val) {
            if (ch == '\'') escaped.push_back('\'');
            escaped.push_back(ch);
        }
        return "'" + escaped + "'";
    }
    if (auto x = std::dynamic_pointer_cast<ast::BoolLit>(value)) {
        return x->val ? "true" : "false";
    }
    return {};
}

std::vector<ColMeta> build_result_schema(const Query &query,
                                         const std::vector<ColMeta> &all_cols) {
    std::vector<ColMeta> result;
    int offset = 0;
    for (const auto &item : query.select_items) {
        ColMeta meta{};
        meta.tab_name.clear();
        meta.name = item.output_name;
        meta.index = false;
        if (!item.expr.is_aggregate) {
            const auto &source = find_col_meta(all_cols, item.expr.col);
            meta.type = source.type;
            meta.len = source.len;
        } else if (item.expr.agg_type == AggFuncType::COUNT) {
            meta.type = TYPE_INT;
            meta.len = sizeof(int);
        } else if (item.expr.agg_type == AggFuncType::AVG) {
            meta.type = TYPE_FLOAT;
            meta.len = sizeof(float);
        } else {
            const auto &source = find_col_meta(all_cols, item.expr.col);
            meta.type = source.type;
            meta.len = source.len;
        }
        meta.offset = offset;
        offset += meta.len;
        result.push_back(std::move(meta));
    }
    return result;
}

}  // namespace

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    auto query = std::make_shared<Query>();

    if (auto x = std::dynamic_pointer_cast<ast::UnionStmt>(parse)) {
        query->is_union = true;
        query->union_alias = x->alias;
        query->select_all = true;
        query->explain_analyze = x->explain_analyze;
        query->limit = x->limit;

        if (x->branches.size() < 2) throw RMDBError("UNION requires at least two branches");
        for (const auto &branch : x->branches) {
            auto analyzed = do_analyze(branch);
            if (analyzed->result_cols.empty()) throw RMDBError("UNION branch has no output columns");
            query->union_branches.push_back(std::move(analyzed));
        }

        const size_t width = query->union_branches.front()->result_cols.size();
        query->result_cols = query->union_branches.front()->result_cols;
        for (size_t branch_idx = 1; branch_idx < query->union_branches.size(); ++branch_idx) {
            const auto &branch_cols = query->union_branches[branch_idx]->result_cols;
            if (branch_cols.size() != width) {
                throw RMDBError("UNION branches must return the same number of columns");
            }
            for (size_t i = 0; i < width; ++i) {
                auto &common = query->result_cols[i];
                const auto &incoming = branch_cols[i];
                if (common.type == incoming.type) {
                    if (common.type == TYPE_STRING) common.len = std::max(common.len, incoming.len);
                    continue;
                }
                const bool numeric =
                    (common.type == TYPE_INT && incoming.type == TYPE_FLOAT) ||
                    (common.type == TYPE_FLOAT && incoming.type == TYPE_INT);
                if (!numeric) {
                    throw IncompatibleTypeError(coltype2str(common.type),
                                                coltype2str(incoming.type));
                }
                common.type = TYPE_FLOAT;
                common.len = sizeof(float);
            }
        }

        int offset = 0;
        for (auto &col : query->result_cols) {
            col.tab_name = query->union_alias;
            col.offset = offset;
            col.index = false;
            offset += col.len;
            query->output_names.push_back(col.name);
        }

        for (const auto &order : x->orders) {
            auto col = std::dynamic_pointer_cast<ast::Col>(order->expr);
            if (!col) throw RMDBError("UNION ORDER BY requires an output column");
            if (!col->tab_name.empty() && col->tab_name != query->union_alias) {
                throw ColumnNotFoundError(col->tab_name + "." + col->col_name);
            }
            int matched = -1;
            for (size_t i = 0; i < query->result_cols.size(); ++i) {
                if (query->result_cols[i].name != col->col_name) continue;
                if (matched != -1) throw AmbiguousColumnError(col->col_name);
                matched = static_cast<int>(i);
            }
            if (matched == -1) throw ColumnNotFoundError(col->col_name);
            query->order_bys.push_back({
                {query->union_alias, query->result_cols[matched].name},
                order->orderby_dir == ast::OrderBy_DESC});
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->table_refs = x->table_refs;
        query->select_all = x->select_all;
        query->explain_analyze = x->explain_analyze;
        query->limit = x->limit;

        std::set<std::string> qualifiers;
        for (const auto &table_ref : query->table_refs) {
            if (!sm_manager_->db_.is_table(table_ref.tab_name)) {
                throw TableNotFoundError(table_ref.tab_name);
            }
            const std::string qualifier = table_ref.qualifier();
            if (!qualifiers.insert(qualifier).second) {
                throw RMDBError("Duplicate table name or alias: " + qualifier);
            }
            query->tables.push_back(qualifier);
        }

        std::vector<ColMeta> all_cols;
        get_all_cols(query->table_refs, all_cols);

        get_clause(x->conds, query->conds);
        check_clause(query->table_refs, query->conds);

        for (const auto &group_col : x->group_by) {
            TabCol col{group_col->tab_name, group_col->col_name};
            col = check_column(all_cols, col);
            const auto &meta = find_col_meta(all_cols, col);
            if (meta.type != TYPE_INT && meta.type != TYPE_FLOAT && meta.type != TYPE_STRING) {
                throw RMDBError("Unsupported GROUP BY column type");
            }
            query->group_cols.push_back(std::move(col));
        }

        if (query->select_all) {
            if (!x->group_by.empty() || !x->having.empty()) {
                throw RMDBError("SELECT * is not valid in an aggregate query");
            }
            for (const auto &col : all_cols) {
                QueryExpr expr;
                expr.col = {col.tab_name, col.name};
                SelectItemInfo item{expr, "", col.name};
                query->select_items.push_back(item);
                query->cols.push_back(expr.col);
                query->output_names.push_back(col.name);
            }
        } else {
            for (const auto &sv_item : x->select_items) {
                SelectItemInfo item;
                item.expr = resolve_query_expr(sv_item->expr, all_cols);
                item.alias = sv_item->alias;
                item.output_name = item.alias.empty() ? default_output_name(item.expr) : item.alias;
                query->has_aggregate = query->has_aggregate || item.expr.is_aggregate;
                if (!item.expr.is_aggregate) query->cols.push_back(item.expr.col);
                query->output_names.push_back(item.output_name);
                query->select_items.push_back(std::move(item));
            }
        }

        query->grouped = query->has_aggregate || !query->group_cols.empty() || !x->having.empty();

        if (query->grouped) {
            for (const auto &item : query->select_items) {
                if (item.expr.is_aggregate) continue;
                const bool grouped = std::any_of(query->group_cols.begin(), query->group_cols.end(),
                                                 [&](const TabCol &col) { return same_col(col, item.expr.col); });
                if (!grouped) {
                    throw RMDBError("Non-aggregate SELECT column must appear in GROUP BY");
                }
            }
        }

        auto resolve_having_expr = [&](const std::shared_ptr<ast::Expr> &expr) {
            if (auto col = std::dynamic_pointer_cast<ast::Col>(expr); col && col->tab_name.empty()) {
                const SelectItemInfo *matched = nullptr;
                for (const auto &item : query->select_items) {
                    if (!item.alias.empty() && item.alias == col->col_name) {
                        if (matched != nullptr) throw AmbiguousColumnError(col->col_name);
                        matched = &item;
                    }
                }
                if (matched != nullptr) return matched->expr;
            }
            return resolve_query_expr(expr, all_cols);
        };

        for (const auto &sv_cond : x->having) {
            HavingCondition cond;
            cond.lhs = resolve_having_expr(sv_cond->lhs);
            cond.op = convert_sv_comp_op(sv_cond->op);
            query->has_aggregate = query->has_aggregate || cond.lhs.is_aggregate;

            if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(sv_cond->rhs)) {
                cond.is_rhs_val = true;
                cond.rhs_val = convert_sv_value(rhs_val);
                const ColType lhs_type = expr_type(cond.lhs, all_cols);
                if (cond.rhs_val.type == TYPE_INT && lhs_type == TYPE_FLOAT) {
                    cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
                } else if (cond.rhs_val.type == TYPE_FLOAT && lhs_type == TYPE_INT) {
                    cond.rhs_val.set_int(static_cast<int>(cond.rhs_val.float_val));
                } else if (cond.rhs_val.type != lhs_type) {
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(cond.rhs_val.type));
                }
            } else {
                cond.is_rhs_val = false;
                cond.rhs_expr = resolve_having_expr(sv_cond->rhs);
                query->has_aggregate = query->has_aggregate || cond.rhs_expr.is_aggregate;
                const auto lhs_type = expr_type(cond.lhs, all_cols);
                const auto rhs_type = expr_type(cond.rhs_expr, all_cols);
                if (lhs_type != rhs_type &&
                    !((lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) ||
                      (lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT))) {
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
                }
            }

            auto validate_plain_having_col = [&](const QueryExpr &expr) {
                if (expr.is_aggregate) return;
                const bool grouped = std::any_of(query->group_cols.begin(), query->group_cols.end(),
                                                 [&](const TabCol &col) { return same_col(col, expr.col); });
                if (!grouped) throw RMDBError("HAVING column must appear in GROUP BY");
            };
            validate_plain_having_col(cond.lhs);
            if (!cond.is_rhs_val) validate_plain_having_col(cond.rhs_expr);
            query->having_conds.push_back(std::move(cond));
        }

        query->grouped = query->has_aggregate || !query->group_cols.empty() || !query->having_conds.empty();
        if (!query->having_conds.empty() && !query->grouped) {
            throw RMDBError("HAVING requires grouping or aggregation");
        }

        for (const auto &order : x->orders) {
            OrderByInfo info;
            info.is_desc = order->orderby_dir == ast::OrderBy_DESC;
            if (query->grouped) {
                int matched = -1;
                if (auto col = std::dynamic_pointer_cast<ast::Col>(order->expr)) {
                    if (col->tab_name.empty()) {
                        for (size_t i = 0; i < query->select_items.size(); ++i) {
                            if (query->select_items[i].alias == col->col_name ||
                                query->select_items[i].output_name == col->col_name) {
                                if (matched != -1) throw AmbiguousColumnError(col->col_name);
                                matched = static_cast<int>(i);
                            }
                        }
                    }
                }
                if (matched == -1) {
                    QueryExpr resolved = resolve_query_expr(order->expr, all_cols);
                    for (size_t i = 0; i < query->select_items.size(); ++i) {
                        if (same_expr(query->select_items[i].expr, resolved)) {
                            matched = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (matched == -1) throw RMDBError("ORDER BY expression must be in aggregate SELECT list");
                info.col = {"", query->select_items[matched].output_name};
            } else {
                auto col = std::dynamic_pointer_cast<ast::Col>(order->expr);
                if (!col) throw RMDBError("Aggregate ORDER BY requires an aggregate query");
                bool alias_match = false;
                if (col->tab_name.empty()) {
                    for (const auto &item : query->select_items) {
                        if (!item.alias.empty() && item.alias == col->col_name) {
                            if (alias_match) throw AmbiguousColumnError(col->col_name);
                            info.col = item.expr.col;
                            alias_match = true;
                        }
                    }
                }
                if (!alias_match) info.col = check_column(all_cols, {col->tab_name, col->col_name});
            }
            query->order_bys.push_back(std::move(info));
        }
        query->result_cols = build_result_schema(*query, all_cols);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        query->tables.push_back(x->tab_name);
        query->table_refs.emplace_back(x->tab_name);
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);

        for (const auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {x->tab_name, sv_set_clause->col_name};
            set_clause.self_ref = sv_set_clause->self_ref;
            if (sv_set_clause->self_ref) {
                set_clause.rhs_col = {x->tab_name, sv_set_clause->source_col};
                set_clause.delta = convert_sv_value(sv_set_clause->delta);
            } else {
                set_clause.rhs = convert_sv_value(sv_set_clause->val);
            }
            query->set_clauses.push_back(std::move(set_clause));
        }

        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &set_clause : query->set_clauses) {
            auto col_it = tab.get_col(set_clause.lhs.col_name);
            if (set_clause.self_ref) {
                auto source_it = tab.get_col(set_clause.rhs_col.col_name);
                if ((col_it->type != TYPE_INT && col_it->type != TYPE_FLOAT) ||
                    (source_it->type != TYPE_INT && source_it->type != TYPE_FLOAT) ||
                    (set_clause.delta.type != TYPE_INT && set_clause.delta.type != TYPE_FLOAT)) {
                    throw IncompatibleTypeError("numeric", "non-numeric");
                }
                continue;
            }
            if (set_clause.rhs.type != col_it->type) {
                if (set_clause.rhs.type == TYPE_INT && col_it->type == TYPE_FLOAT) {
                    set_clause.rhs.set_float(static_cast<float>(set_clause.rhs.int_val));
                } else if (set_clause.rhs.type == TYPE_FLOAT && col_it->type == TYPE_INT) {
                    set_clause.rhs.set_int(static_cast<int>(set_clause.rhs.float_val));
                } else {
                    throw IncompatibleTypeError(coltype2str(col_it->type), coltype2str(set_clause.rhs.type));
                }
            }
            set_clause.rhs.init_raw(col_it->len);
        }
        get_clause(x->conds, query->conds);
        check_clause(query->table_refs, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        query->tables.push_back(x->tab_name);
        query->table_refs.emplace_back(x->tab_name);
        get_clause(x->conds, query->conds);
        check_clause(query->table_refs, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        query->tables.push_back(x->tab_name);
        query->table_refs.emplace_back(x->tab_name);
        for (const auto &sv_val : x->vals) query->values.push_back(convert_sv_value(sv_val));
        const auto &cols = sm_manager_->db_.get_table(x->tab_name).cols;
        if (query->values.size() != cols.size()) throw InvalidValueCountError();
        for (size_t i = 0; i < query->values.size(); ++i) {
            auto &value = query->values[i];
            const auto &col = cols[i];
            if (value.type == TYPE_INT && col.type == TYPE_FLOAT) {
                value.set_float(static_cast<float>(value.int_val));
            } else if (value.type == TYPE_FLOAT && col.type == TYPE_INT) {
                value.set_int(static_cast<int>(value.float_val));
            } else if (value.type != col.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(value.type));
            }
            value.init_raw(col.len);
        }
    }

    query->parse = std::move(parse);
    return query;
}

QueryExpr Analyze::resolve_query_expr(const std::shared_ptr<ast::Expr> &expr,
                                      const std::vector<ColMeta> &all_cols) {
    QueryExpr result;
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr)) {
        result.col = check_column(all_cols, {col->tab_name, col->col_name});
        return result;
    }
    auto agg = std::dynamic_pointer_cast<ast::AggExpr>(expr);
    if (!agg) throw RMDBError("Expected a column or aggregate expression");
    result.is_aggregate = true;
    result.aggregate_star = agg->star;
    switch (agg->agg_type) {
        case ast::AGG_COUNT: result.agg_type = AggFuncType::COUNT; break;
        case ast::AGG_MAX: result.agg_type = AggFuncType::MAX; break;
        case ast::AGG_MIN: result.agg_type = AggFuncType::MIN; break;
        case ast::AGG_SUM: result.agg_type = AggFuncType::SUM; break;
        case ast::AGG_AVG: result.agg_type = AggFuncType::AVG; break;
    }
    if (agg->star) {
        if (result.agg_type != AggFuncType::COUNT) throw RMDBError("Only COUNT supports *");
        return result;
    }
    if (!agg->col) throw RMDBError("Aggregate function requires a column");
    result.col = check_column(all_cols, {agg->col->tab_name, agg->col->col_name});
    const ColType type = find_col_meta(all_cols, result.col).type;
    if (result.agg_type == AggFuncType::COUNT) {
        if (type != TYPE_INT && type != TYPE_FLOAT && type != TYPE_STRING) {
            throw RMDBError("Unsupported COUNT column type");
        }
    } else if (type != TYPE_INT && type != TYPE_FLOAT) {
        throw RMDBError("MAX, MIN, SUM and AVG require a numeric column");
    }
    return result;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        std::string qualifier;
        for (const auto &col : all_cols) {
            if (col.name != target.col_name) continue;
            if (!qualifier.empty() && qualifier != col.tab_name) throw AmbiguousColumnError(target.col_name);
            qualifier = col.tab_name;
        }
        if (qualifier.empty()) throw ColumnNotFoundError(target.col_name);
        target.tab_name = qualifier;
        return target;
    }
    (void)find_col_meta(all_cols, target);
    return target;
}

void Analyze::get_all_cols(const std::vector<ast::TableRef> &table_refs,
                           std::vector<ColMeta> &all_cols) {
    for (const auto &table_ref : table_refs) {
        const auto &physical_cols = sm_manager_->db_.get_table(table_ref.tab_name).cols;
        for (auto col : physical_cols) {
            col.tab_name = table_ref.qualifier();
            all_cols.push_back(std::move(col));
        }
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds,
                         std::vector<Condition> &conds) {
    conds.clear();
    for (const auto &expr : sv_conds) {
        auto lhs_col = std::dynamic_pointer_cast<ast::Col>(expr->lhs);
        if (!lhs_col) throw RMDBError("Aggregate functions are not allowed in WHERE");
        Condition cond;
        cond.lhs_col = {lhs_col->tab_name, lhs_col->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_display = ast_value_to_display(rhs_val);
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {rhs_col->tab_name, rhs_col->col_name};
        } else {
            throw RMDBError("Aggregate functions are not allowed in WHERE");
        }
        conds.push_back(std::move(cond));
    }
}

void Analyze::check_clause(const std::vector<ast::TableRef> &table_refs,
                           std::vector<Condition> &conds) {
    std::vector<ColMeta> all_cols;
    get_all_cols(table_refs, all_cols);

    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) cond.rhs_col = check_column(all_cols, cond.rhs_col);

        const ColMeta &lhs_col = find_col_meta(all_cols, cond.lhs_col);
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (cond.rhs_val.type == TYPE_INT && lhs_col.type == TYPE_FLOAT) {
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            } else if (cond.rhs_val.type == TYPE_FLOAT && lhs_col.type == TYPE_INT) {
                cond.rhs_val.set_int(static_cast<int>(cond.rhs_val.float_val));
            }
            cond.rhs_val.init_raw(lhs_col.len);
            rhs_type = cond.rhs_val.type;
        } else {
            rhs_type = find_col_meta(all_cols, cond.rhs_col).type;
        }

        if (lhs_col.type != rhs_type &&
            !((lhs_col.type == TYPE_INT && rhs_type == TYPE_FLOAT) ||
              (lhs_col.type == TYPE_FLOAT && rhs_type == TYPE_INT))) {
            throw IncompatibleTypeError(coltype2str(lhs_col.type), coltype2str(rhs_type));
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto x = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) val.set_int(x->val);
    else if (auto x = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) val.set_float(x->val);
    else if (auto x = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) val.set_str(x->val);
    else if (auto x = std::dynamic_pointer_cast<ast::BoolLit>(sv_val)) val.set_int(x->val ? 1 : 0);
    else throw InternalError("Unexpected value type");
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    static const std::map<ast::SvCompOp, CompOp> mapping = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return mapping.at(op);
}
