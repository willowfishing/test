/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "planner.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>


namespace {

bool tab_col_less(const TabCol &lhs, const TabCol &rhs) {
    return std::tie(lhs.tab_name, lhs.col_name) <
           std::tie(rhs.tab_name, rhs.col_name);
}

void add_needed(std::map<std::string, std::set<std::string>> &needed,
                const TabCol &col) {
    if (!col.tab_name.empty()) needed[col.tab_name].insert(col.col_name);
}

std::vector<TabCol> sorted_cols_for(
    const std::string &qualifier,
    const std::set<std::string> &names,
    const std::vector<ColMeta> &physical_cols) {
    std::vector<TabCol> result;
    for (const auto &col : physical_cols) {
        if (names.count(col.name) != 0) result.push_back({qualifier, col.name});
    }
    std::sort(result.begin(), result.end(), tab_col_less);
    return result;
}


}  // namespace

bool Planner::get_index_cols(const std::string &physical_table,
                             const std::string &qualifier,
                             const std::vector<Condition> &curr_conds,
                             std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    TabMeta &tab = sm_manager_->db_.get_table(physical_table);

    size_t best_prefix = 0;
    size_t best_width = std::numeric_limits<size_t>::max();
    const IndexMeta *best_index = nullptr;

    for (const auto &index : tab.indexes) {
        size_t prefix = 0;
        for (const auto &index_col : index.cols) {
            bool equality = false;
            bool range = false;
            for (const auto &cond : curr_conds) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != qualifier ||
                    cond.lhs_col.col_name != index_col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    equality = true;
                } else if (cond.op == OP_LT || cond.op == OP_LE ||
                           cond.op == OP_GT || cond.op == OP_GE) {
                    range = true;
                }
            }
            if (equality) {
                ++prefix;
                continue;
            }
            if (range) ++prefix;
            break;
        }
        if (prefix > best_prefix ||
            (prefix == best_prefix && prefix > 0 &&
             index.cols.size() < best_width)) {
            best_prefix = prefix;
            best_width = index.cols.size();
            best_index = &index;
        }
    }

    if (best_index == nullptr || best_prefix == 0) return false;
    for (const auto &col : best_index->cols) {
        index_col_names.push_back(col.name);
    }
    return true;
}

std::shared_ptr<Query> Planner::logical_optimization(
    std::shared_ptr<Query> query, Context *context) {
    (void)context;
    // Predicate and projection pushdown are materialized while building the
    // canonical four-node plan tree in generate_select_plan().
    return query;
}

std::shared_ptr<Plan> Planner::generate_sort_plan(
    const std::shared_ptr<Query> &query, std::shared_ptr<Plan> plan) {
    if (query->order_bys.empty()) return plan;
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_bys);
}

std::shared_ptr<Plan> Planner::generate_select_plan(
    const std::shared_ptr<Query> &input_query, Context *context) {
    auto query = logical_optimization(input_query, context);
    const bool explain = query->explain_analyze;
    const bool mvcc_snapshot = context != nullptr && context->txn_ != nullptr &&
                               context->txn_->get_txn_mode();

    std::map<std::string, std::vector<Condition>> local_conds;
    std::vector<Condition> join_conds;
    for (const auto &cond : query->conds) {
        if (cond.is_rhs_val || cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
            local_conds[cond.lhs_col.tab_name].push_back(cond);
        } else {
            join_conds.push_back(cond);
        }
    }

    // A table-side Project is above its pushed-down Filter, so columns used
    // only by that Filter have already been consumed. Keep only columns
    // required by ancestor nodes: SELECT, Join, and ORDER BY.
    std::map<std::string, std::set<std::string>> needed_cols;
    if (!query->select_all) {
        for (const auto &item : query->select_items) {
            if (!item.expr.aggregate_star) add_needed(needed_cols, item.expr.col);
        }
        for (const auto &col : query->group_cols) add_needed(needed_cols, col);
        for (const auto &cond : query->having_conds) {
            if (!cond.lhs.aggregate_star) add_needed(needed_cols, cond.lhs.col);
            if (!cond.is_rhs_val && !cond.rhs_expr.aggregate_star) {
                add_needed(needed_cols, cond.rhs_expr.col);
            }
        }
        for (const auto &cond : join_conds) {
            add_needed(needed_cols, cond.lhs_col);
            add_needed(needed_cols, cond.rhs_col);
        }
        if (!query->grouped) {
            for (const auto &order : query->order_bys) add_needed(needed_cols, order.col);
        }
    }

    auto has_single_column_index = [&](const std::string &physical,
                                       const std::string &column) {
        const auto &indexes = sm_manager_->db_.get_table(physical).indexes;
        return std::any_of(indexes.begin(), indexes.end(), [&](const IndexMeta &index) {
            return index.col_num == 1 && index.cols.size() == 1 &&
                   index.cols.front().name == column;
        });
    };

    auto make_table_plan = [&](const ast::TableRef &table_ref,
                               const std::vector<Condition> &lookup_conds,
                               const std::vector<std::string> &lookup_index_cols) {
        const std::string physical = table_ref.tab_name;
        const std::string qualifier = table_ref.qualifier();
        const auto &conds = local_conds[qualifier];
        const bool parameterized = !lookup_conds.empty();

        std::shared_ptr<Plan> node;
        if (explain) {
            node = std::make_shared<ScanPlan>(
                parameterized ? T_IndexScan : T_SeqScan, sm_manager_, physical,
                table_ref.alias, std::vector<Condition>{}, lookup_index_cols,
                lookup_conds);
            if (!conds.empty()) {
                node = std::make_shared<FilterPlan>(node, conds);
            }
        } else if (parameterized && !mvcc_snapshot) {
            node = std::make_shared<ScanPlan>(
                T_IndexScan, sm_manager_, physical, table_ref.alias, conds,
                lookup_index_cols, lookup_conds);
        } else {
            std::vector<std::string> index_cols;
            const bool use_index = !mvcc_snapshot &&
                get_index_cols(physical, qualifier, conds, index_cols);
            node = std::make_shared<ScanPlan>(
                use_index ? T_IndexScan : T_SeqScan, sm_manager_, physical,
                table_ref.alias, conds, std::move(index_cols));
        }

        if (query->table_refs.size() > 1 && !query->select_all) {
            const auto &all_cols = sm_manager_->db_.get_table(physical).cols;
            auto projected = sorted_cols_for(qualifier, needed_cols[qualifier],
                                             all_cols);
            // EXPLAIN ANALYZE in task 7 specifies an explicit Project above
            // every Join input, even when the required set happens to contain
            // all physical columns of that table.
            node = std::make_shared<ProjectionPlan>(
                T_Projection, node, projected, false, projected);
        }
        return node;
    };

    if (query->table_refs.empty()) {
        throw InternalError("SELECT has no input table");
    }

    // Task 7 defines the physical Join shape as a fixed SQL-order left-deep
    // tree: ((table1 JOIN table2) JOIN table3) ... .  The first table is always
    // the initial outer/driver input and each newly encountered table is the
    // right-side inner input of the next Join node.
    std::shared_ptr<Plan> root =
        make_table_plan(query->table_refs.front(), {}, {});
    std::set<std::string> joined{query->table_refs.front().qualifier()};
    std::vector<bool> used(join_conds.size(), false);

    for (size_t table_idx = 1; table_idx < query->table_refs.size(); ++table_idx) {
        const auto &right_ref = query->table_refs[table_idx];
        const std::string right_name = right_ref.qualifier();
        std::vector<Condition> current_join;
        std::vector<size_t> current_indexes;
        for (size_t j = 0; j < join_conds.size(); ++j) {
            if (used[j]) continue;
            const auto &cond = join_conds[j];
            const bool lhs_left = joined.count(cond.lhs_col.tab_name) != 0;
            const bool rhs_left = joined.count(cond.rhs_col.tab_name) != 0;
            const bool lhs_right = cond.lhs_col.tab_name == right_name;
            const bool rhs_right = cond.rhs_col.tab_name == right_name;
            if ((lhs_left && rhs_right) || (rhs_left && lhs_right)) {
                current_join.push_back(cond);
                current_indexes.push_back(j);
            }
        }

        std::vector<Condition> lookup_conds;
        std::vector<std::string> lookup_index_cols;
        for (const auto &cond : current_join) {
            if (cond.op != OP_EQ || cond.is_rhs_val) continue;
            std::string inner_col;
            if (cond.lhs_col.tab_name == right_name &&
                joined.count(cond.rhs_col.tab_name) != 0) {
                inner_col = cond.lhs_col.col_name;
            } else if (cond.rhs_col.tab_name == right_name &&
                       joined.count(cond.lhs_col.tab_name) != 0) {
                inner_col = cond.rhs_col.col_name;
            } else {
                continue;
            }
            if (has_single_column_index(right_ref.tab_name, inner_col)) {
                lookup_conds.push_back(cond);
                lookup_index_cols.push_back(inner_col);
                break;  // one unique point-lookup index is sufficient
            }
        }

        auto right = make_table_plan(right_ref, lookup_conds,
                                     lookup_index_cols);
        root = std::make_shared<JoinPlan>(T_NestLoop, root, std::move(right),
                                          current_join);
        for (size_t index : current_indexes) used[index] = true;
        joined.insert(right_name);
    }

    // A predicate whose two referenced tables only became available after its
    // nominal JOIN level is still evaluated at the completed root, preserving
    // correctness without changing the mandated left-deep table order.
    std::vector<Condition> remaining;
    for (size_t i = 0; i < join_conds.size(); ++i) {
        if (!used[i]) remaining.push_back(join_conds[i]);
    }
    if (!remaining.empty()) {
        if (auto join = std::dynamic_pointer_cast<JoinPlan>(root)) {
            join->conds_.insert(join->conds_.end(), remaining.begin(),
                                remaining.end());
        } else {
            root = std::make_shared<FilterPlan>(root, std::move(remaining));
        }
    }

    if (query->grouped) {
        root = std::make_shared<AggregatePlan>(std::move(root), query->select_items,
                                               query->group_cols, query->having_conds);
        root = generate_sort_plan(query, std::move(root));
        if (query->limit >= 0) {
            root = std::make_shared<LimitPlan>(std::move(root),
                                               static_cast<size_t>(query->limit));
        }
        return root;
    }

    root = generate_sort_plan(query, std::move(root));
    std::vector<TabCol> display_cols = query->cols;
    std::sort(display_cols.begin(), display_cols.end(), tab_col_less);
    root = std::make_shared<ProjectionPlan>(T_Projection, std::move(root),
                                            query->cols, query->select_all,
                                            std::move(display_cols),
                                            query->output_names);
    if (query->limit >= 0) {
        root = std::make_shared<LimitPlan>(std::move(root),
                                           static_cast<size_t>(query->limit));
    }
    return root;
}

std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query,
                                          Context *context) {
    std::shared_ptr<Plan> root;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        std::vector<ColDef> defs;
        for (const auto &field : x->fields) {
            auto col = std::dynamic_pointer_cast<ast::ColDef>(field);
            if (!col) throw InternalError("Unexpected field type");
            defs.push_back({col->col_name, interp_sv_type(col->type_len->type),
                            col->type_len->len});
        }
        root = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name,
                                         std::vector<std::string>{}, defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        root = std::make_shared<DDLPlan>(T_DropTable, x->tab_name,
                                         std::vector<std::string>{},
                                         std::vector<ColDef>{});
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        root = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name,
                                         x->col_names,
                                         std::vector<ColDef>{});
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        root = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names,
                                         std::vector<ColDef>{});
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        root = std::make_shared<DMLPlan>(T_Insert, nullptr, x->tab_name,
                                         query->values,
                                         std::vector<Condition>{},
                                         std::vector<SetClause>{});
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        std::vector<std::string> index_cols;
        const bool use_index = !(context != nullptr && context->txn_ != nullptr &&
                                 context->txn_->get_txn_mode()) &&
                               get_index_cols(x->tab_name, x->tab_name,
                                              query->conds, index_cols);
        auto scan = std::make_shared<ScanPlan>(
            use_index ? T_IndexScan : T_SeqScan, sm_manager_, x->tab_name,
            query->conds, std::move(index_cols));
        root = std::make_shared<DMLPlan>(T_Delete, scan, x->tab_name,
                                         std::vector<Value>{}, query->conds,
                                         std::vector<SetClause>{});
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        std::vector<std::string> index_cols;
        const bool use_index = !(context != nullptr && context->txn_ != nullptr &&
                                 context->txn_->get_txn_mode()) &&
                               get_index_cols(x->tab_name, x->tab_name,
                                              query->conds, index_cols);
        auto scan = std::make_shared<ScanPlan>(
            use_index ? T_IndexScan : T_SeqScan, sm_manager_, x->tab_name,
            query->conds, std::move(index_cols));
        root = std::make_shared<DMLPlan>(T_Update, scan, x->tab_name,
                                         std::vector<Value>{}, query->conds,
                                         query->set_clauses);
    } else if (std::dynamic_pointer_cast<ast::UnionStmt>(query->parse)) {
        std::vector<std::shared_ptr<Plan>> branches;
        branches.reserve(query->union_branches.size());
        for (const auto &branch : query->union_branches) {
            branches.push_back(generate_select_plan(branch, context));
        }
        std::shared_ptr<Plan> union_root = std::make_shared<UnionPlan>(
            std::move(branches), query->result_cols);
        union_root = generate_sort_plan(query, std::move(union_root));
        if (query->limit >= 0) {
            union_root = std::make_shared<LimitPlan>(
                std::move(union_root), static_cast<size_t>(query->limit));
        }
        root = std::make_shared<DMLPlan>(
            query->explain_analyze ? T_ExplainSelect : T_select, union_root,
            std::string{}, std::vector<Value>{}, std::vector<Condition>{},
            std::vector<SetClause>{});
    } else if (std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        auto projection = generate_select_plan(query, context);
        root = std::make_shared<DMLPlan>(
            query->explain_analyze ? T_ExplainSelect : T_select, projection,
            std::string{}, std::vector<Value>{}, std::vector<Condition>{},
            std::vector<SetClause>{});
    } else {
        throw InternalError("Unexpected AST root");
    }
    return root;
}
