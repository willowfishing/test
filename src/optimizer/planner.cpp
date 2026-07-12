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

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "record_printer.h"

namespace {
std::string col_to_string(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name.empty() ? col->col_name : col->tab_name + "." + col->col_name;
}

std::string value_to_string(const std::shared_ptr<ast::Value> &value) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::to_string(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", float_lit->val);
        return buf;
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return "'" + str_lit->val + "'";
    }
    return "";
}

std::string op_to_string(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return "=";
        case ast::SV_OP_NE: return "<>";
        case ast::SV_OP_LT: return "<";
        case ast::SV_OP_GT: return ">";
        case ast::SV_OP_LE: return "<=";
        case ast::SV_OP_GE: return ">=";
    }
    return "";
}

std::string cond_to_string(const std::shared_ptr<ast::BinaryExpr> &cond) {
    std::string rhs;
    if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
        rhs = col_to_string(rhs_col);
    } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
        rhs = value_to_string(rhs_val);
    }
    return col_to_string(cond->lhs) + op_to_string(cond->op) + rhs;
}

std::string join_tables_string(const std::set<std::string> &tables) {
    std::string out;
    bool first = true;
    for (auto &table : tables) {
        if (!first) {
            out += ", ";
        }
        out += table;
        first = false;
    }
    return out;
}

std::string project_columns_string(std::vector<std::string> cols) {
    std::sort(cols.begin(), cols.end());
    std::string out;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += cols[i];
    }
    return out;
}

bool table_has_filter(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    for (auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            return true;
        }
    }
    return false;
}

std::string ast_col_key(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name + "\x1f" + col->col_name;
}

std::string condition_col_key(const TabCol &col) {
    return col.tab_name + "\x1f" + col.col_name;
}

std::string condition_key(const Condition &cond) {
    std::string key = condition_col_key(cond.lhs_col) + "\x1e" + std::to_string(static_cast<int>(cond.op)) + "\x1e";
    if (!cond.is_rhs_val) {
        return key + condition_col_key(cond.rhs_col);
    }
    key += "v" + std::to_string(static_cast<int>(cond.rhs_val.type)) + "\x1e";
    if (cond.rhs_val.type == TYPE_INT) {
        key += std::to_string(cond.rhs_val.int_val);
    } else if (cond.rhs_val.type == TYPE_FLOAT) {
        key += std::to_string(cond.rhs_val.float_val);
    } else {
        key += cond.rhs_val.str_val;
    }
    return key;
}

void propagate_equal_join_value_filters(std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    std::map<std::string, std::shared_ptr<ast::Col>> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (!rhs_col || cond->op != ast::SV_OP_EQ || cond->lhs->tab_name == rhs_col->tab_name) {
            continue;
        }
        std::string lhs_key = ast_col_key(cond->lhs);
        std::string rhs_key = ast_col_key(rhs_col);
        key_to_col[lhs_key] = cond->lhs;
        key_to_col[rhs_key] = rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<std::shared_ptr<ast::Col>>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) {
            continue;
        }
        std::vector<std::string> stack{entry.first};
        std::vector<std::shared_ptr<ast::Col>> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) {
                    stack.push_back(next);
                }
            }
        }
        for (const auto &col : cols) {
            component_cols[ast_col_key(col)] = cols;
        }
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) {
        existing.insert(cond_to_string(cond));
    }
    std::vector<std::shared_ptr<ast::BinaryExpr>> additions;
    for (const auto &cond : conds) {
        if (!std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            continue;
        }
        auto comp_it = component_cols.find(ast_col_key(cond->lhs));
        if (comp_it == component_cols.end()) {
            continue;
        }
        for (const auto &target_col : comp_it->second) {
            if (target_col->tab_name == cond->lhs->tab_name && target_col->col_name == cond->lhs->col_name) {
                continue;
            }
            auto derived_lhs = std::make_shared<ast::Col>(target_col->tab_name, target_col->col_name);
            auto derived = std::make_shared<ast::BinaryExpr>(derived_lhs, cond->op, cond->rhs);
            if (existing.insert(cond_to_string(derived)).second) {
                additions.push_back(std::move(derived));
            }
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

void propagate_equal_join_value_filters(std::vector<Condition> &conds) {
    std::map<std::string, TabCol> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ || cond.lhs_col.tab_name == cond.rhs_col.tab_name) {
            continue;
        }
        std::string lhs_key = condition_col_key(cond.lhs_col);
        std::string rhs_key = condition_col_key(cond.rhs_col);
        key_to_col[lhs_key] = cond.lhs_col;
        key_to_col[rhs_key] = cond.rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<TabCol>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) {
            continue;
        }
        std::vector<std::string> stack{entry.first};
        std::vector<TabCol> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) {
                    stack.push_back(next);
                }
            }
        }
        for (const auto &col : cols) {
            component_cols[condition_col_key(col)] = cols;
        }
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) {
        existing.insert(condition_key(cond));
    }
    std::vector<Condition> additions;
    for (const auto &cond : conds) {
        if (!cond.is_rhs_val) {
            continue;
        }
        auto comp_it = component_cols.find(condition_col_key(cond.lhs_col));
        if (comp_it == component_cols.end()) {
            continue;
        }
        for (const auto &target_col : comp_it->second) {
            if (target_col.tab_name == cond.lhs_col.tab_name && target_col.col_name == cond.lhs_col.col_name) {
                continue;
            }
            Condition derived = cond;
            derived.lhs_col = target_col;
            if (existing.insert(condition_key(derived)).second) {
                additions.push_back(std::move(derived));
            }
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

std::string table_filter_string(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds, const std::string &visible) {
    std::vector<std::string> filters;
    for (auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) || (rhs_col && rhs_col->tab_name == visible))) {
            filters.push_back(cond_to_string(cond));
        }
    }
    std::sort(filters.begin(), filters.end());
    std::string out;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += filters[i];
    }
    return out;
}

std::vector<std::string> make_explain_lines(const std::shared_ptr<ast::SelectStmt> &select, SmManager *sm_manager) {
    std::map<std::string, std::string> alias_to_table;
    std::vector<std::string> input_visible;
    std::vector<std::string> input_tables;
    for (auto &ref : select->table_refs) {
        alias_to_table[ref->visible_name()] = ref->tab_name;
        input_visible.push_back(ref->visible_name());
        input_tables.push_back(ref->tab_name);
    }
    if (input_visible.size() == 1) {
        const std::string &default_alias = input_visible.front();
        for (auto &item : select->select_items) {
            if (item->col && item->col->tab_name.empty()) {
                item->col->tab_name = default_alias;
            }
        }
        for (auto &cond : select->conds) {
            if (cond->lhs->tab_name.empty()) {
                cond->lhs->tab_name = default_alias;
            }
            if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
                if (rhs_col->tab_name.empty()) {
                    rhs_col->tab_name = default_alias;
                }
            }
        }
    }
    auto explain_conds = select->conds;
    propagate_equal_join_value_filters(explain_conds);

    auto table_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            rows++;
        }
        return rows;
    };

    auto table_has_join_index = [&](const std::string &alias, const std::shared_ptr<ast::BinaryExpr> &cond) {
        std::vector<std::string> cols;
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == alias) {
            cols.push_back(cond->lhs->col_name);
        } else if (rhs_col && rhs_col->tab_name == alias) {
            cols.push_back(rhs_col->col_name);
        }
        if (cols.empty()) {
            return false;
        }
        auto &tab = sm_manager->db_.get_table(alias_to_table[alias]);
        return tab.is_index(cols);
    };

    auto read_col = [&](const std::string &alias, const Rid &rid, const std::string &col_name) {
        auto &tab = sm_manager->db_.get_table(alias_to_table[alias]);
        auto col = tab.get_col(col_name);
        auto rec = sm_manager->fhs_.at(alias_to_table[alias])->get_record(rid, nullptr);
        std::string raw(rec->data + col->offset, col->len);
        return std::pair<ColMeta, std::string>(*col, raw);
    };

    auto compare_raw = [&](const ColMeta &lhs_col, const std::string &lhs_raw,
                           const ColMeta &rhs_col, const std::string &rhs_raw,
                           ast::SvCompOp op) {
        int cmp = 0;
        if (lhs_col.type == TYPE_FLOAT || rhs_col.type == TYPE_FLOAT) {
            float lhs = lhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(lhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(lhs_raw.data()));
            float rhs = rhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(rhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(rhs_raw.data()));
            cmp = (lhs > rhs) - (lhs < rhs);
        } else if (lhs_col.type == TYPE_INT) {
            int lhs = *reinterpret_cast<const int *>(lhs_raw.data());
            int rhs = *reinterpret_cast<const int *>(rhs_raw.data());
            cmp = (lhs > rhs) - (lhs < rhs);
        } else {
            std::string lhs = lhs_raw;
            std::string rhs = rhs_raw;
            lhs.resize(strlen(lhs.c_str()));
            rhs.resize(strlen(rhs.c_str()));
            cmp = lhs.compare(rhs);
        }
        switch (op) {
            case ast::SV_OP_EQ: return cmp == 0;
            case ast::SV_OP_NE: return cmp != 0;
            case ast::SV_OP_LT: return cmp < 0;
            case ast::SV_OP_GT: return cmp > 0;
            case ast::SV_OP_LE: return cmp <= 0;
            case ast::SV_OP_GE: return cmp >= 0;
        }
        return false;
    };

    auto compare_value_cond = [&](const std::string &alias, const Rid &rid,
                                  const std::shared_ptr<ast::BinaryExpr> &cond) {
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        ColMeta rhs_col = lhs_col;
        std::string rhs_raw(lhs_col.len, '\0');
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(cond->rhs)) {
            int value = int_lit->val;
            if (lhs_col.type == TYPE_FLOAT) {
                float f = static_cast<float>(value);
                memcpy(rhs_raw.data(), &f, sizeof(float));
                rhs_col.type = TYPE_FLOAT;
                rhs_col.len = sizeof(float);
            } else {
                memcpy(rhs_raw.data(), &value, sizeof(int));
                rhs_col.type = TYPE_INT;
                rhs_col.len = sizeof(int);
            }
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(cond->rhs)) {
            float value = float_lit->val;
            memcpy(rhs_raw.data(), &value, sizeof(float));
            rhs_col.type = TYPE_FLOAT;
            rhs_col.len = sizeof(float);
        } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(cond->rhs)) {
            rhs_raw.assign(lhs_col.len, '\0');
            memcpy(rhs_raw.data(), str_lit->val.c_str(), std::min<int>(lhs_col.len, str_lit->val.size()));
            rhs_col.type = TYPE_STRING;
        }
        return compare_raw(lhs_col, lhs_raw, rhs_col, rhs_raw, cond->op);
    };

    std::map<std::string, std::vector<std::shared_ptr<ast::BinaryExpr>>> local_conds;
    std::vector<std::shared_ptr<ast::BinaryExpr>> join_conds;
    for (auto &cond : explain_conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (rhs_col && rhs_col->tab_name != cond->lhs->tab_name) {
            join_conds.push_back(cond);
        } else {
            local_conds[cond->lhs->tab_name].push_back(cond);
        }
    }
    auto compare_filter_cond = [&](const std::string &alias, const Rid &rid,
                                   const std::shared_ptr<ast::BinaryExpr> &cond) {
        if (std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
            return compare_value_cond(alias, rid, cond);
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        auto [rhs_meta, rhs_raw] = read_col(alias, rid, rhs_col->col_name);
        return compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op);
    };

    auto filtered_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            bool pass = true;
            for (auto &cond : local_conds[alias]) {
                if (!compare_filter_cond(alias, scan.rid(), cond)) {
                    pass = false;
                    break;
                }
            }
            if (pass) {
                rows++;
            }
        }
        return rows;
    };

    auto join_count = [&](const std::set<std::string> &aliases) {
        std::vector<std::string> order;
        for (auto &alias : input_visible) {
            if (aliases.count(alias)) {
                order.push_back(alias);
            }
        }
        std::map<std::string, Rid> current;
        int rows = 0;
        std::function<void(size_t)> dfs = [&](size_t idx) {
            if (idx == order.size()) {
                for (auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) {
                            continue;
                        }
                        auto [lhs_col, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                            rhs_col->col_name);
                        if (!compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op)) {
                            return;
                        }
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                rows++;
                return;
            }
            auto &alias = order[idx];
            auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(idx + 1);
            }
        };
        dfs(0);
        return rows;
    };

    auto prefix_records = [&](const std::set<std::string> &aliases) {
        std::vector<std::string> order;
        for (auto &alias : input_visible) {
            if (aliases.count(alias)) {
                order.push_back(alias);
            }
        }
        std::vector<std::map<std::string, Rid>> records;
        std::map<std::string, Rid> current;
        std::function<void(size_t)> dfs = [&](size_t idx) {
            if (idx == order.size()) {
                for (auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) {
                            continue;
                        }
                        auto [lhs_col, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                            rhs_col->col_name);
                        if (!compare_raw(lhs_col, lhs_raw, rhs_meta, rhs_raw, cond->op)) {
                            return;
                        }
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                records.push_back(current);
                return;
            }
            auto &alias = order[idx];
            auto fh = sm_manager->fhs_.at(alias_to_table[alias]).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(idx + 1);
            }
        };
        dfs(0);
        return records;
    };

    auto find_index_cond = [&](const std::string &right_alias,
                               const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds)
        -> std::shared_ptr<ast::BinaryExpr> {
        for (auto &cond : conds) {
            if (cond->op == ast::SV_OP_EQ && table_has_join_index(right_alias, cond)) {
                return cond;
            }
        }
        return nullptr;
    };

    auto index_hit_count = [&](const std::set<std::string> &outer_aliases, const std::string &right_alias,
                               const std::shared_ptr<ast::BinaryExpr> &index_cond) {
        if (index_cond == nullptr) {
            return 0;
        }
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
        std::shared_ptr<ast::Col> right_col;
        std::shared_ptr<ast::Col> outer_col;
        if (index_cond->lhs->tab_name == right_alias && rhs_col && outer_aliases.count(rhs_col->tab_name)) {
            right_col = index_cond->lhs;
            outer_col = rhs_col;
        } else if (rhs_col && rhs_col->tab_name == right_alias && outer_aliases.count(index_cond->lhs->tab_name)) {
            right_col = rhs_col;
            outer_col = index_cond->lhs;
        } else {
            return 0;
        }

        int rows = 0;
        auto prefixes = prefix_records(outer_aliases);
        auto right_fh = sm_manager->fhs_.at(alias_to_table[right_alias]).get();
        for (auto &prefix : prefixes) {
            auto [outer_meta, outer_raw] = read_col(outer_col->tab_name, prefix[outer_col->tab_name], outer_col->col_name);
            for (RmScan scan(right_fh); !scan.is_end(); scan.next()) {
                auto [right_meta, right_raw] = read_col(right_alias, scan.rid(), right_col->col_name);
                if (!compare_raw(right_meta, right_raw, outer_meta, outer_raw, ast::SV_OP_EQ)) {
                    continue;
                }
                bool pass_local = true;
                for (auto &cond : local_conds[right_alias]) {
                    if (!compare_filter_cond(right_alias, scan.rid(), cond)) {
                        pass_local = false;
                        break;
                    }
                }
                if (pass_local) {
                    rows++;
                }
            }
        }
        return rows;
    };

    std::vector<std::string> lines;
    std::set<std::string> all_aliases(input_visible.begin(), input_visible.end());
    int final_rows = input_visible.empty() ? 0 : join_count(all_aliases);
    int project_rows = select->has_limit && select->limit >= 0 ? std::min(final_rows, select->limit) : final_rows;
    if (select->select_items.empty()) {
        lines.push_back("Project(columns=[*], rows=" + std::to_string(project_rows) + ")");
    } else {
        std::vector<std::string> cols;
        for (auto &item : select->select_items) {
            if (!item->is_agg && item->col) {
                cols.push_back(col_to_string(item->col));
            }
        }
        lines.push_back("Project(columns=[" + project_columns_string(cols) + "], rows=" + std::to_string(project_rows) + ")");
    }

    std::set<std::string> joined_aliases;
    std::map<std::string, int> leaf_multiplier;
    std::map<std::string, int> leaf_output_rows;
    std::map<std::string, std::shared_ptr<ast::BinaryExpr>> leaf_index_cond;
    std::map<std::string, bool> leaf_index;
    std::map<std::string, int> leaf_depth;
    std::vector<std::string> leaf_order;
    std::vector<std::string> join_lines;
    if (!join_conds.empty()) {
        struct JoinStep {
            std::set<std::string> aliases;
            std::vector<std::shared_ptr<ast::BinaryExpr>> conds;
        };
        std::vector<JoinStep> join_steps;

        int table_count = static_cast<int>(input_visible.size());
        if (!input_visible.empty()) {
            const std::string &first_alias = input_visible.front();
            leaf_multiplier[first_alias] = 1;
            leaf_output_rows[first_alias] = filtered_rows(first_alias);
            leaf_index[first_alias] = false;
            leaf_depth[first_alias] = table_count;
            leaf_order.push_back(first_alias);
            joined_aliases.insert(first_alias);
        }

        for (size_t i = 1; i < input_visible.size(); ++i) {
            const std::string &right_alias = input_visible[i];
            std::vector<std::shared_ptr<ast::BinaryExpr>> step_conds;
            for (auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (!rhs_col) {
                    continue;
                }
                bool lhs_right = cond->lhs->tab_name == right_alias && joined_aliases.count(rhs_col->tab_name);
                bool rhs_right = rhs_col->tab_name == right_alias && joined_aliases.count(cond->lhs->tab_name);
                if (lhs_right || rhs_right) {
                    step_conds.push_back(cond);
                }
            }
            int prefix_rows = join_count(joined_aliases);
            leaf_multiplier[right_alias] = prefix_rows;
            leaf_index_cond[right_alias] = find_index_cond(right_alias, step_conds);
            leaf_index[right_alias] = leaf_index_cond[right_alias] != nullptr;
            leaf_depth[right_alias] = table_count - static_cast<int>(i) + 1;
            if (leaf_index[right_alias]) {
                leaf_output_rows[right_alias] = index_hit_count(joined_aliases, right_alias, leaf_index_cond[right_alias]);
            } else {
                leaf_output_rows[right_alias] = filtered_rows(right_alias) * prefix_rows;
            }
            leaf_order.push_back(right_alias);
            joined_aliases.insert(right_alias);
            if (!step_conds.empty()) {
                join_steps.push_back({joined_aliases, step_conds});
            }
        }
        for (auto &step : join_steps) {
            std::set<std::string> joined_tables;
            for (auto &alias : step.aliases) {
                joined_tables.insert(alias_to_table[alias]);
            }
            std::vector<std::string> conds;
            for (auto &cond : step.conds) {
                conds.push_back(cond_to_string(cond));
            }
            std::sort(conds.begin(), conds.end());
            int join_rows = join_count(step.aliases);
            join_lines.push_back("Join(tables=[" + join_tables_string(joined_tables) + "], condition=[" +
                                 project_columns_string(conds) + "], rows=" + std::to_string(join_rows) + ")");
        }
        for (int i = static_cast<int>(join_lines.size()) - 1; i >= 0; --i) {
            int depth = static_cast<int>(join_lines.size()) - i;
            lines.push_back(std::string(depth, '\t') + join_lines[i]);
        }
    }

    bool has_filters = false;
    for (auto &visible : input_visible) {
        has_filters = has_filters || table_has_filter(explain_conds, visible);
    }
    if (join_conds.empty() && has_filters) {
        std::sort(leaf_order.begin(), leaf_order.end(), [&](const std::string &lhs, const std::string &rhs) {
            return alias_to_table[lhs] < alias_to_table[rhs];
        });
    }
    for (auto &visible : input_visible) {
        if (std::find(leaf_order.begin(), leaf_order.end(), visible) == leaf_order.end()) {
            leaf_order.push_back(visible);
        }
    }

    for (auto &visible : leaf_order) {
        std::vector<std::string> projected;
        if (!select->select_items.empty()) {
            for (auto &item : select->select_items) {
                if (!item->is_agg && item->col && item->col->tab_name == visible) {
                    projected.push_back(col_to_string(item->col));
                }
            }
            for (auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (cond->lhs->tab_name == visible) {
                    projected.push_back(col_to_string(cond->lhs));
                }
                if (rhs_col->tab_name == visible) {
                    projected.push_back(col_to_string(rhs_col));
                }
            }
            std::sort(projected.begin(), projected.end());
            projected.erase(std::unique(projected.begin(), projected.end()), projected.end());
            if (!projected.empty() && !join_conds.empty()) {
                int base_rows = leaf_output_rows.count(visible) ? leaf_output_rows[visible] : filtered_rows(visible);
                lines.push_back(std::string(leaf_depth[visible], '\t') + "Project(columns=[" +
                                project_columns_string(projected) + "], rows=" + std::to_string(base_rows) + ")");
            }
        }
        std::string filter = table_filter_string(explain_conds, visible);
        if (!filter.empty()) {
            int filter_rows = filtered_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
            int filter_depth = join_conds.empty() ? 1 : leaf_depth[visible] + (projected.empty() ? 0 : 1);
            lines.push_back(std::string(filter_depth, '\t') + "Filter(condition=[" + filter +
                            "], rows=" + std::to_string(filter_rows) + ")");
        }
        int scan_rows = leaf_index[visible] ? leaf_output_rows[visible]
                                            : table_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
        std::string scan = "Scan(table=" + alias_to_table[visible] + ", type=" +
                           (leaf_index[visible] ? "IndexScan" : "SeqScan");
        if (leaf_index[visible]) {
            auto index_cond = leaf_index_cond[visible];
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
            if (index_cond->lhs->tab_name == visible) {
                scan += ", using_index=(" + index_cond->lhs->col_name + ")";
            } else if (rhs_col && rhs_col->tab_name == visible) {
                scan += ", using_index=(" + rhs_col->col_name + ")";
            }
        }
        scan += ", rows=" + std::to_string(scan_rows) + ")";
        int scan_depth = 0;
        if (join_conds.empty()) {
            scan_depth = filter.empty() ? 1 : 2;
        } else {
            scan_depth = leaf_depth[visible] + (projected.empty() ? 0 : 1) + (filter.empty() ? 0 : 1);
        }
        lines.push_back(std::string(scan_depth, '\t') + scan);
    }
    return lines;
}
}

bool Planner::get_index_cols(std::string tab_name, std::string visible_name, std::vector<Condition> curr_conds,
                             std::vector<std::string>& index_col_names) {
    index_col_names.clear();
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    size_t best_prefix = 0;
    std::vector<std::string> best_index_cols;
    for (auto &index : tab.indexes) {
        size_t prefix = 0;
        for (auto &index_col : index.cols) {
            auto cond_it = std::find_if(curr_conds.begin(), curr_conds.end(), [&](const Condition &cond) {
                return cond.is_rhs_val && cond.lhs_col.tab_name == visible_name &&
                       cond.lhs_col.col_name == index_col.name && cond.op != OP_NE;
            });
            if (cond_it == curr_conds.end()) {
                break;
            }
            prefix++;
            if (cond_it->op != OP_EQ) {
                break;
            }
        }
        if (prefix > best_prefix) {
            best_prefix = prefix;
            best_index_cols.clear();
            for (auto &col : index.cols) {
                best_index_cols.push_back(col.name);
            }
        }
    }
    if (best_prefix == 0) {
        return false;
    }
    index_col_names = std::move(best_index_cols);
    return true;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->visible_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->visible_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
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
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->visible_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}

std::vector<std::string> get_join_index_cols(SmManager *sm_manager, const std::string &right_table,
                                             const std::string &right_visible,
                                             const std::vector<Condition> &join_conds) {
    auto &tab = sm_manager->db_.get_table(right_table);
    for (auto &cond : join_conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        std::vector<std::string> cols;
        if (cond.rhs_col.tab_name == right_visible) {
            cols.push_back(cond.rhs_col.col_name);
        } else if (cond.lhs_col.tab_name == right_visible) {
            cols.push_back(cond.lhs_col.col_name);
        }
        if (!cols.empty() && tab.is_index(cols)) {
            return cols;
        }
    }
    return {};
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
    auto base_table = [&](const std::string &visible) -> const std::string & {
        auto it = query->alias_to_table.find(visible);
        return it == query->alias_to_table.end() ? visible : it->second;
    };
    propagate_equal_join_value_filters(query->conds);
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        const std::string &base = base_table(tables[i]);
        bool index_exist = get_index_cols(base, tables[i], curr_conds, index_col_names);
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base, curr_conds, index_col_names, tables[i]);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, base, curr_conds, index_col_names, tables[i]);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1) {
        return table_scan_executors[0];
    }
    auto conds = std::move(query->conds);
    std::map<CompOp, CompOp> swap_op = {
        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
    };
    std::shared_ptr<Plan> table_join_executors = table_scan_executors[0];
    std::set<std::string> joined_tables{tables[0]};

    for (size_t i = 1; i < tables.size(); ++i) {
        const std::string &right_table = tables[i];
        std::vector<Condition> join_conds;
        auto it = conds.begin();
        while (it != conds.end()) {
            if (!it->is_rhs_val &&
                ((joined_tables.count(it->lhs_col.tab_name) && it->rhs_col.tab_name == right_table) ||
                 (it->lhs_col.tab_name == right_table && joined_tables.count(it->rhs_col.tab_name)))) {
                Condition cond = *it;
                if (cond.lhs_col.tab_name == right_table) {
                    std::swap(cond.lhs_col, cond.rhs_col);
                    cond.op = swap_op.at(cond.op);
                }
                join_conds.push_back(cond);
                it = conds.erase(it);
            } else {
                ++it;
            }
        }
        auto index_col_names = get_join_index_cols(sm_manager_, base_table(right_table), right_table, join_conds);
        PlanTag join_tag = index_col_names.empty() ? T_NestLoop : T_IndexNestLoop;
        table_join_executors = std::make_shared<JoinPlan>(join_tag, std::move(table_join_executors),
                                                          table_scan_executors[i], join_conds, index_col_names);
        joined_tables.insert(right_table);
    }

    for (auto &cond : conds) {
        push_conds(&cond, table_join_executors);
    }

    return table_join_executors;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    if(query->order_cols.empty()) {
        return plan;
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_cols);
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
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    if (query->has_aggregate || query->has_group) {
        plannerRoot = std::make_shared<AggregatePlan>(std::move(plannerRoot), query->select_items,
                                                      query->group_cols, query->having_conds, sel_cols);
    } else {
        plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), 
                                                            std::move(sel_cols));
    }
    if (query->has_limit) {
        plannerRoot = std::make_shared<LimitPlan>(std::move(plannerRoot), query->limit);
    }

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
        bool index_exist = get_index_cols(x->tab_name, x->tab_name, query->conds, index_col_names);
        
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
        bool index_exist = get_index_cols(x->tab_name, x->tab_name, query->conds, index_col_names);

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
    } else if (auto x = std::dynamic_pointer_cast<ast::ExplainStmt>(query->parse)) {
        plannerRoot = std::make_shared<ExplainPlan>(make_explain_lines(x->select, sm_manager_));
    } else if (auto x = std::dynamic_pointer_cast<ast::UnionStmt>(query->parse)) {
        std::vector<std::shared_ptr<Plan>> subplans;
        for (auto &subquery : query->union_queries) {
            subplans.push_back(generate_select_plan(subquery, context));
        }
        std::shared_ptr<Plan> union_plan = std::make_shared<UnionPlan>(std::move(subplans), query->union_cols);
        if (!query->order_cols.empty()) {
            union_plan = std::make_shared<SortPlan>(T_Sort, std::move(union_plan), query->order_cols);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_select, union_plan, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        if (!query->union_queries.empty()) {
            std::vector<std::shared_ptr<Plan>> subplans;
            for (auto &subquery : query->union_queries) {
                subplans.push_back(generate_select_plan(subquery, context));
            }
            std::shared_ptr<Plan> union_plan = std::make_shared<UnionPlan>(std::move(subplans), query->union_cols);
            if (!query->order_cols.empty()) {
                union_plan = std::make_shared<SortPlan>(T_Sort, std::move(union_plan), query->order_cols);
            }
            plannerRoot = std::make_shared<DMLPlan>(T_select, union_plan, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
            return plannerRoot;
        }

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection;
        if (query->is_semi_join) {
            std::vector<std::string> empty_index;
            auto base_table = [&](const std::string &visible) -> const std::string & {
                auto it = query->alias_to_table.find(visible);
                return it == query->alias_to_table.end() ? visible : it->second;
            };
            auto left_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[0]),
                                                        std::vector<Condition>(), empty_index, query->tables[0]);
            auto right_scan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, base_table(query->tables[1]),
                                                         std::vector<Condition>(), empty_index, query->tables[1]);
            auto semi = std::make_shared<SemiJoinPlan>(left_scan, right_scan, query->semi_conds);
            projection = std::make_shared<ProjectionPlan>(T_Projection, semi, query->cols);
        } else {
            projection = generate_select_plan(std::move(query), context);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(query->parse)) {
        plannerRoot = std::make_shared<OtherPlan>(T_ShowIndex, x->tab_name);
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
