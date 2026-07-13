#pragma once

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyze/analyze.h"
#include "executor_abstract.h"
#include "record/rm_scan.h"
#include "system/sm.h"

enum class ExplainNodeType { Scan, Filter, Project, Join };

struct ExplainTupleSet {
    std::vector<ColMeta> cols;
    std::vector<std::unique_ptr<RmRecord>> rows;
};

struct ExplainNode {
    ExplainNodeType type;
    size_t rows{0};
    std::string real_table;
    std::string display_table;
    bool project_all{false};
    bool use_index{false};
    std::string using_index_col;
    std::vector<std::string> tables;
    std::vector<TabCol> columns;
    std::vector<Condition> conditions;
    std::vector<std::unique_ptr<ExplainNode>> children;
};

class ExplainAnalyzeRunner {
   public:
    static std::string Run(SmManager *sm_manager, const std::shared_ptr<Query> &query) {
        ExplainAnalyzeRunner runner(sm_manager, query);
        auto root = runner.BuildPlan();
        runner.ResetRows(root.get());
        (void)runner.Execute(root.get());
        return runner.Format(root.get(), 0);
    }

   private:
    SmManager *sm_manager_;
    std::shared_ptr<Query> query_;
    std::unordered_map<std::string, std::vector<Condition>> single_table_conds_;
    std::vector<Condition> join_conds_;
    std::unordered_map<std::string, std::set<std::string>> needed_cols_;

    ExplainAnalyzeRunner(SmManager *sm_manager, std::shared_ptr<Query> query)
        : sm_manager_(sm_manager), query_(std::move(query)) {}

    std::unique_ptr<ExplainNode> BuildPlan() {
        SplitConditions();
        CollectNeededColumns();

        std::vector<size_t> table_order = ChooseTableOrder();
        std::vector<std::unique_ptr<ExplainNode>> leaves;
        leaves.reserve(table_order.size());
        for (auto i : table_order) {
            leaves.push_back(BuildLeaf(query_->real_tables[i], query_->display_tables[i]));
        }

        std::unique_ptr<ExplainNode> current = std::move(leaves[0]);
        std::set<std::string> joined{query_->display_tables[table_order[0]]};
        std::vector<bool> used_join_conds(join_conds_.size(), false);

        for (size_t i = 1; i < leaves.size(); ++i) {
            std::set<std::string> next_joined = joined;
            next_joined.insert(query_->display_tables[table_order[i]]);

            auto join = std::make_unique<ExplainNode>();
            join->type = ExplainNodeType::Join;
            join->children.push_back(std::move(current));
            join->children.push_back(std::move(leaves[i]));

            for (size_t j = 0; j < join_conds_.size(); ++j) {
                if (used_join_conds[j]) {
                    continue;
                }
                auto cond_tables = ConditionTables(join_conds_[j]);
                bool ready = std::all_of(cond_tables.begin(), cond_tables.end(),
                                         [&](const std::string &tab) { return next_joined.count(tab) > 0; });
                if (ready) {
                    join->conditions.push_back(join_conds_[j]);
                    used_join_conds[j] = true;
                }
            }
            join->tables = RealTablesFor(next_joined, table_order, i + 1);

            current = std::move(join);
            joined = std::move(next_joined);
        }

        auto root = std::make_unique<ExplainNode>();
        root->type = ExplainNodeType::Project;
        root->project_all = query_->select_all;
        root->columns = query_->cols;
        std::sort(root->columns.begin(), root->columns.end());
        root->children.push_back(std::move(current));
        return root;
    }

    std::vector<size_t> ChooseTableOrder() {
        std::vector<size_t> order;
        const size_t table_count = query_->display_tables.size();
        if (query_->preserve_join_order || table_count <= 1) {
            for (size_t i = 0; i < table_count; ++i) {
                order.push_back(i);
            }
            return order;
        }

        std::vector<bool> used(table_count, false);
        auto choose_best = [&](const std::vector<size_t> &candidates) {
            return *std::min_element(candidates.begin(), candidates.end(), [&](size_t lhs, size_t rhs) {
                bool lhs_filtered = HasSingleTableFilter(query_->display_tables[lhs]);
                bool rhs_filtered = HasSingleTableFilter(query_->display_tables[rhs]);
                if (lhs_filtered != rhs_filtered) {
                    return lhs_filtered;
                }
                size_t lhs_rows = EstimateFilteredRows(lhs);
                size_t rhs_rows = EstimateFilteredRows(rhs);
                if (lhs_rows != rhs_rows) {
                    return lhs_rows < rhs_rows;
                }
                return query_->display_tables[lhs] < query_->display_tables[rhs];
            });
        };

        std::vector<size_t> candidates;
        for (size_t i = 0; i < table_count; ++i) {
            candidates.push_back(i);
        }
        size_t first = choose_best(candidates);
        order.push_back(first);
        used[first] = true;
        std::set<std::string> joined{query_->display_tables[first]};

        while (order.size() < table_count) {
            candidates.clear();
            for (size_t i = 0; i < table_count; ++i) {
                if (!used[i] && HasJoinWith(query_->display_tables[i], joined)) {
                    candidates.push_back(i);
                }
            }
            if (candidates.empty()) {
                for (size_t i = 0; i < table_count; ++i) {
                    if (!used[i]) {
                        candidates.push_back(i);
                    }
                }
            }
            size_t next = choose_best(candidates);
            order.push_back(next);
            used[next] = true;
            joined.insert(query_->display_tables[next]);
        }
        return order;
    }

    bool HasSingleTableFilter(const std::string &display_table) const {
        auto it = single_table_conds_.find(display_table);
        return it != single_table_conds_.end() && !it->second.empty();
    }

    bool HasJoinWith(const std::string &display_table, const std::set<std::string> &joined) const {
        for (auto &cond : join_conds_) {
            auto tables = ConditionTables(cond);
            if (tables.count(display_table) == 0) {
                continue;
            }
            for (auto &table : tables) {
                if (table != display_table && joined.count(table) > 0) {
                    return true;
                }
            }
        }
        return false;
    }

    size_t EstimateFilteredRows(size_t table_idx) const {
        const std::string &real_table = query_->real_tables[table_idx];
        const std::string &display_table = query_->display_tables[table_idx];
        TabMeta &tab = sm_manager_->db_.get_table(real_table);
        std::vector<ColMeta> cols = tab.cols;
        for (auto &col : cols) {
            col.tab_name = display_table;
        }
        auto cond_it = single_table_conds_.find(display_table);
        RmFileHandle *fh = sm_manager_->fhs_.at(real_table).get();
        size_t rows = 0;
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            auto rec = fh->get_record(scan.rid(), nullptr);
            if (cond_it == single_table_conds_.end() ||
                AbstractExecutor::eval_conds(cols, rec.get(), cond_it->second)) {
                rows++;
            }
        }
        return rows;
    }

    std::unique_ptr<ExplainNode> BuildLeaf(const std::string &real_table, const std::string &display_table) {
        auto scan = std::make_unique<ExplainNode>();
        scan->type = ExplainNodeType::Scan;
        scan->real_table = real_table;
        scan->display_table = display_table;

        std::unique_ptr<ExplainNode> node = std::move(scan);
        auto cond_it = single_table_conds_.find(display_table);
        if (cond_it != single_table_conds_.end() && !cond_it->second.empty()) {
            auto filter = std::make_unique<ExplainNode>();
            filter->type = ExplainNodeType::Filter;
            filter->conditions = cond_it->second;
            filter->children.push_back(std::move(node));
            node = std::move(filter);
        }

        if (!query_->select_all && query_->real_tables.size() > 1 && ShouldPushProject(real_table, display_table)) {
            auto project = std::make_unique<ExplainNode>();
            project->type = ExplainNodeType::Project;
            for (auto &col_name : needed_cols_[display_table]) {
                project->columns.push_back(TabCol{.tab_name = display_table, .col_name = col_name});
            }
            std::sort(project->columns.begin(), project->columns.end());
            project->children.push_back(std::move(node));
            node = std::move(project);
        }
        return node;
    }

    bool ShouldPushProject(const std::string &real_table, const std::string &display_table) const {
        auto needed_it = needed_cols_.find(display_table);
        if (needed_it == needed_cols_.end()) {
            return false;
        }
        return !needed_it->second.empty();
    }

    void SplitConditions() {
        for (auto &cond : query_->conds) {
            auto tables = ConditionTables(cond);
            if (tables.size() == 1) {
                single_table_conds_[*tables.begin()].push_back(cond);
            } else {
                join_conds_.push_back(cond);
            }
        }
        for (auto &entry : single_table_conds_) {
            std::sort(entry.second.begin(), entry.second.end(),
                      [&](const Condition &a, const Condition &b) { return ConditionToString(a) < ConditionToString(b); });
        }
        std::sort(join_conds_.begin(), join_conds_.end(),
                  [&](const Condition &a, const Condition &b) { return ConditionToString(a) < ConditionToString(b); });
    }

    void CollectNeededColumns() {
        if (query_->select_all) {
            return;
        }
        for (auto &col : query_->cols) {
            needed_cols_[col.tab_name].insert(col.col_name);
        }
        for (auto &cond : query_->conds) {
            needed_cols_[cond.lhs_col.tab_name].insert(cond.lhs_col.col_name);
            if (!cond.is_rhs_val) {
                needed_cols_[cond.rhs_col.tab_name].insert(cond.rhs_col.col_name);
            }
        }
        for (auto &display : query_->display_tables) {
            (void)needed_cols_[display];
        }
    }

    ExplainTupleSet Execute(ExplainNode *node) {
        switch (node->type) {
            case ExplainNodeType::Scan:
                return ExecuteScan(node);
            case ExplainNodeType::Filter:
                return ExecuteFilter(node);
            case ExplainNodeType::Project:
                return ExecuteProject(node);
            case ExplainNodeType::Join:
                return ExecuteJoin(node);
        }
        return {};
    }

    ExplainTupleSet ExecuteScan(ExplainNode *node) {
        ExplainTupleSet result;
        TabMeta &tab = sm_manager_->db_.get_table(node->real_table);
        result.cols = tab.cols;
        for (auto &col : result.cols) {
            col.tab_name = node->display_table;
        }
        RmFileHandle *fh = sm_manager_->fhs_.at(node->real_table).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            node->rows++;
            result.rows.push_back(fh->get_record(scan.rid(), nullptr));
        }
        return result;
    }

    ExplainTupleSet ExecuteFilter(ExplainNode *node) {
        ExplainTupleSet child = Execute(node->children[0].get());
        ExplainTupleSet result;
        result.cols = child.cols;
        for (auto &rec : child.rows) {
            if (AbstractExecutor::eval_conds(result.cols, rec.get(), node->conditions)) {
                result.rows.push_back(std::make_unique<RmRecord>(*rec));
                node->rows++;
            }
        }
        return result;
    }

    ExplainTupleSet ExecuteProject(ExplainNode *node) {
        ExplainTupleSet child = Execute(node->children[0].get());
        if (node->project_all) {
            node->rows += child.rows.size();
            return child;
        }

        ExplainTupleSet result;
        size_t len = 0;
        std::vector<size_t> indexes;
        for (auto &target : node->columns) {
            auto it = std::find_if(child.cols.begin(), child.cols.end(), [&](const ColMeta &col) {
                return col.tab_name == target.tab_name && col.name == target.col_name;
            });
            if (it == child.cols.end()) {
                throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
            }
            indexes.push_back(static_cast<size_t>(it - child.cols.begin()));
            auto col = *it;
            col.offset = static_cast<int>(len);
            len += col.len;
            result.cols.push_back(col);
        }

        for (auto &rec : child.rows) {
            auto projected = std::make_unique<RmRecord>(static_cast<int>(len));
            for (size_t i = 0; i < indexes.size(); ++i) {
                const auto &src_col = child.cols[indexes[i]];
                memcpy(projected->data + result.cols[i].offset, rec->data + src_col.offset, src_col.len);
            }
            result.rows.push_back(std::move(projected));
            node->rows++;
        }
        return result;
    }

    ExplainTupleSet ExecuteJoin(ExplainNode *node) {
        ExplainTupleSet left = Execute(node->children[0].get());
        ExplainTupleSet result;
        size_t left_len = TupleLen(left.cols);
        result.cols = JoinCols(left.cols, OutputCols(node->children[1].get()), left_len);

        std::string index_col;
        bool use_index = CanUseIndexLookup(node->children[1].get(), left.cols, node->conditions, index_col);
        ExplainTupleSet indexed_right;
        if (use_index) {
            indexed_right = Execute(node->children[1].get());
            ResetRows(node->children[1].get());
            MarkIndexScan(node->children[1].get(), index_col);
        }

        for (auto &left_rec : left.rows) {
            ExplainTupleSet right;
            if (use_index) {
                right = CloneTupleSet(indexed_right);
            } else {
                right = Execute(node->children[1].get());
            }
            std::vector<ColMeta> joined_cols = JoinCols(left.cols, right.cols, left_len);
            size_t right_len = TupleLen(right.cols);
            result.cols = joined_cols;

            for (auto &right_rec : right.rows) {
                auto joined = std::make_unique<RmRecord>(static_cast<int>(left_len + right_len));
                memcpy(joined->data, left_rec->data, left_len);
                memcpy(joined->data + left_len, right_rec->data, right_len);
                if (AbstractExecutor::eval_conds(joined_cols, joined.get(), node->conditions)) {
                    if (use_index) {
                        IncrementRows(node->children[1].get());
                    }
                    result.rows.push_back(std::move(joined));
                    node->rows++;
                }
            }
        }
        return result;
    }

    ExplainTupleSet CloneTupleSet(const ExplainTupleSet &input) const {
        ExplainTupleSet output;
        output.cols = input.cols;
        for (auto &rec : input.rows) {
            output.rows.push_back(std::make_unique<RmRecord>(*rec));
        }
        return output;
    }

    ExplainNode *FindScanNode(ExplainNode *node) const {
        if (node->type == ExplainNodeType::Scan) {
            return node;
        }
        for (auto &child : node->children) {
            if (auto scan = FindScanNode(child.get())) {
                return scan;
            }
        }
        return nullptr;
    }

    bool CanUseIndexLookup(ExplainNode *right_node, const std::vector<ColMeta> &left_cols,
                           const std::vector<Condition> &conds, std::string &index_col) const {
        ExplainNode *scan = FindScanNode(right_node);
        if (scan == nullptr) {
            return false;
        }
        TabMeta &right_tab = sm_manager_->db_.get_table(scan->real_table);
        for (auto &cond : conds) {
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                continue;
            }
            TabCol right_col;
            TabCol left_col;
            if (cond.lhs_col.tab_name == scan->display_table) {
                right_col = cond.lhs_col;
                left_col = cond.rhs_col;
            } else if (cond.rhs_col.tab_name == scan->display_table) {
                right_col = cond.rhs_col;
                left_col = cond.lhs_col;
            } else {
                continue;
            }
            bool left_has_col = std::any_of(left_cols.begin(), left_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == left_col.tab_name && col.name == left_col.col_name;
            });
            if (left_has_col && right_tab.is_index({right_col.col_name})) {
                index_col = right_col.col_name;
                return true;
            }
        }
        return false;
    }

    void MarkIndexScan(ExplainNode *node, const std::string &index_col) {
        if (node->type == ExplainNodeType::Scan) {
            node->use_index = true;
            node->using_index_col = index_col;
        }
        for (auto &child : node->children) {
            MarkIndexScan(child.get(), index_col);
        }
    }

    void IncrementRows(ExplainNode *node) {
        node->rows++;
        for (auto &child : node->children) {
            IncrementRows(child.get());
        }
    }
    std::vector<ColMeta> OutputCols(ExplainNode *node) const {
        switch (node->type) {
            case ExplainNodeType::Scan: {
                std::vector<ColMeta> cols = sm_manager_->db_.get_table(node->real_table).cols;
                for (auto &col : cols) {
                    col.tab_name = node->display_table;
                }
                return cols;
            }
            case ExplainNodeType::Filter:
                return OutputCols(node->children[0].get());
            case ExplainNodeType::Project: {
                std::vector<ColMeta> child_cols = OutputCols(node->children[0].get());
                if (node->project_all) {
                    return child_cols;
                }
                std::vector<ColMeta> cols;
                size_t offset = 0;
                for (auto &target : node->columns) {
                    auto it = std::find_if(child_cols.begin(), child_cols.end(), [&](const ColMeta &col) {
                        return col.tab_name == target.tab_name && col.name == target.col_name;
                    });
                    if (it == child_cols.end()) {
                        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
                    }
                    auto col = *it;
                    col.offset = static_cast<int>(offset);
                    offset += col.len;
                    cols.push_back(col);
                }
                return cols;
            }
            case ExplainNodeType::Join: {
                std::vector<ColMeta> left_cols = OutputCols(node->children[0].get());
                return JoinCols(left_cols, OutputCols(node->children[1].get()), TupleLen(left_cols));
            }
        }
        return {};
    }

    static std::vector<ColMeta> JoinCols(const std::vector<ColMeta> &left_cols,
                                         const std::vector<ColMeta> &right_cols,
                                         size_t left_len) {
        std::vector<ColMeta> cols = left_cols;
        for (auto col : right_cols) {
            col.offset += static_cast<int>(left_len);
            cols.push_back(col);
        }
        return cols;
    }

    static size_t TupleLen(const std::vector<ColMeta> &cols) {
        size_t len = 0;
        for (auto &col : cols) {
            len = std::max(len, static_cast<size_t>(col.offset + col.len));
        }
        return len;
    }

    void ResetRows(ExplainNode *node) {
        node->rows = 0;
        for (auto &child : node->children) {
            ResetRows(child.get());
        }
    }

    std::set<std::string> ConditionTables(const Condition &cond) const {
        std::set<std::string> tables{cond.lhs_col.tab_name};
        if (!cond.is_rhs_val) {
            tables.insert(cond.rhs_col.tab_name);
        }
        return tables;
    }

    std::vector<std::string> RealTablesFor(const std::set<std::string> &display_tables,
                                           const std::vector<size_t> &table_order,
                                           size_t joined_count) const {
        std::vector<std::string> tables;
        for (size_t i = 0; i < joined_count; ++i) {
            const std::string &display = query_->display_tables[table_order[i]];
            if (display_tables.count(display) == 0) {
                continue;
            }
            auto it = query_->display_to_real.find(display);
            tables.push_back(it == query_->display_to_real.end() ? display : it->second);
        }
        return tables;
    }

    std::string Format(ExplainNode *node, int depth) {
        std::string line(static_cast<size_t>(depth), '\t');
        switch (node->type) {
            case ExplainNodeType::Scan:
                if (node->use_index) {
                    line += "Scan(table=" + node->real_table + ", type=IndexScan, using_index=(" +
                            node->using_index_col + "), rows=" + std::to_string(node->rows) + ")\n";
                } else {
                    line += "Scan(table=" + node->real_table + ", type=SeqScan, rows=" + std::to_string(node->rows) + ")\n";
                }
                break;
            case ExplainNodeType::Filter:
                line += "Filter(condition=[" + JoinStrings(ConditionStrings(node->conditions)) + "], rows=" +
                        std::to_string(node->rows) + ")\n";
                break;
            case ExplainNodeType::Project:
                line += "Project(columns=[" + ProjectColumns(node) + "], rows=" + std::to_string(node->rows) + ")\n";
                break;
            case ExplainNodeType::Join:
                line += "Join(tables=[" + JoinStrings(node->tables) + "], condition=[" +
                        JoinStrings(ConditionStrings(node->conditions)) + "], rows=" + std::to_string(node->rows) +
                        ")\n";
                break;
        }
        for (auto &child : node->children) {
            line += Format(child.get(), depth + 1);
        }
        return line;
    }

    std::string ProjectColumns(ExplainNode *node) const {
        if (node->project_all) {
            return "*";
        }
        std::vector<std::string> cols;
        for (auto &col : node->columns) {
            cols.push_back(col.tab_name + "." + col.col_name);
        }
        std::sort(cols.begin(), cols.end());
        return JoinStrings(cols);
    }

    std::vector<std::string> ConditionStrings(const std::vector<Condition> &conds) const {
        std::vector<std::string> values;
        for (auto &cond : conds) {
            values.push_back(ConditionToString(cond));
        }
        std::sort(values.begin(), values.end());
        return values;
    }

    std::string ConditionToString(const Condition &cond) const {
        std::string result = cond.lhs_col.tab_name + "." + cond.lhs_col.col_name + OpToString(cond.op);
        if (cond.is_rhs_val) {
            result += ValueToString(cond.rhs_val);
        } else {
            result += cond.rhs_col.tab_name + "." + cond.rhs_col.col_name;
        }
        return result;
    }

    static std::string JoinStrings(const std::vector<std::string> &values) {
        std::string result;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                result += ", ";
            }
            result += values[i];
        }
        return result;
    }

    static std::string OpToString(CompOp op) {
        switch (op) {
            case OP_EQ:
                return "=";
            case OP_NE:
                return "<>";
            case OP_LT:
                return "<";
            case OP_GT:
                return ">";
            case OP_LE:
                return "<=";
            case OP_GE:
                return ">=";
        }
        return "";
    }

    static std::string ValueToString(const Value &value) {
        if (value.type == TYPE_INT) {
            return value.raw_text.empty() ? std::to_string(value.int_val) : value.raw_text;
        }
        if (value.type == TYPE_FLOAT) {
            if (!value.raw_text.empty()) {
                return value.raw_text;
            }
            std::ostringstream os;
            os << value.float_val;
            return os.str();
        }
        return "'" + value.str_val + "'";
    }
};
