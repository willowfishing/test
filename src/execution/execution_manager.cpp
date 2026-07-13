/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"
#include "recovery/log_recovery.h"

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>


namespace {

std::string comp_op_to_string(CompOp op) {
    switch (op) {
        case OP_EQ: return "=";
        case OP_NE: return "<>";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LE: return "<=";
        case OP_GE: return ">=";
    }
    return "?";
}

std::string value_to_string(const Value &value) {
    if (value.type == TYPE_INT) return std::to_string(value.int_val);
    if (value.type == TYPE_FLOAT) {
        std::ostringstream out;
        out << std::defaultfloat << value.float_val;
        return out.str();
    }
    std::string escaped;
    escaped.reserve(value.str_val.size());
    for (char ch : value.str_val) {
        if (ch == '\'') escaped.push_back('\'');
        escaped.push_back(ch);
    }
    return "'" + escaped + "'";
}

std::string col_to_string(const TabCol &col) {
    return col.tab_name + "." + col.col_name;
}

std::string condition_to_string(const Condition &cond) {
    std::string result = col_to_string(cond.lhs_col) + comp_op_to_string(cond.op);
    result += cond.is_rhs_val
                  ? (cond.rhs_display.empty() ? value_to_string(cond.rhs_val)
                                              : cond.rhs_display)
                  : col_to_string(cond.rhs_col);
    return result;
}

template <typename T>
std::string join_strings(const std::vector<T> &items, const std::string &sep) {
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out << sep;
        out << items[i];
    }
    return out.str();
}

std::vector<std::string> sorted_condition_strings(
    const std::vector<Condition> &conds) {
    std::vector<std::string> result;
    result.reserve(conds.size());
    for (const auto &cond : conds) result.push_back(condition_to_string(cond));
    std::sort(result.begin(), result.end());
    return result;
}

void collect_physical_tables(const std::shared_ptr<Plan> &plan,
                             std::set<std::string> &tables) {
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        tables.insert(scan->tab_name_);
    } else if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        collect_physical_tables(filter->subplan_, tables);
    } else if (auto project = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        collect_physical_tables(project->subplan_, tables);
    } else if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        collect_physical_tables(join->left_, tables);
        collect_physical_tables(join->right_, tables);
    } else if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        collect_physical_tables(sort->subplan_, tables);
    } else if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        collect_physical_tables(aggregate->subplan_, tables);
    } else if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        collect_physical_tables(limit->subplan_, tables);
    }
}

void format_plan_tree(const std::shared_ptr<Plan> &plan, int depth,
                      std::ostringstream &out) {
    if (auto project = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        std::vector<std::string> columns;
        if (project->display_all_) {
            columns.push_back("*");
        } else {
            for (const auto &col : project->display_cols_) {
                columns.push_back(col_to_string(col));
            }
            std::sort(columns.begin(), columns.end());
        }
        out << std::string(depth, '\t') << "Project(columns=["
            << join_strings(columns, ", ") << "], rows="
            << project->runtime_->rows << ")\n";
        format_plan_tree(project->subplan_, depth + 1, out);
        return;
    }
    if (auto filter = std::dynamic_pointer_cast<FilterPlan>(plan)) {
        auto conditions = sorted_condition_strings(filter->conds_);
        out << std::string(depth, '\t') << "Filter(condition=["
            << join_strings(conditions, ", ") << "], rows="
            << filter->runtime_->rows << ")\n";
        format_plan_tree(filter->subplan_, depth + 1, out);
        return;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        out << std::string(depth, '\t') << "Scan(table=" << scan->tab_name_;
        if (scan->tag == T_IndexScan) {
            out << ", type=IndexScan, using_index=("
                << join_strings(scan->index_col_names_, ",") << ")";
        } else {
            out << ", type=SeqScan";
        }
        out << ", rows=" << scan->runtime_->rows << ")\n";
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        std::set<std::string> table_set;
        collect_physical_tables(plan, table_set);
        std::vector<std::string> tables(table_set.begin(), table_set.end());
        auto conditions = sorted_condition_strings(join->conds_);
        out << std::string(depth, '\t') << "Join(tables=["
            << join_strings(tables, ", ") << "], condition=["
            << join_strings(conditions, ", ") << "], rows="
            << join->runtime_->rows << ")\n";
        format_plan_tree(join->left_, depth + 1, out);
        format_plan_tree(join->right_, depth + 1, out);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        // EXPLAIN ANALYZE for task 4 has only four printable node types.
        format_plan_tree(sort->subplan_, depth, out);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        format_plan_tree(aggregate->subplan_, depth, out);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        format_plan_tree(limit->subplan_, depth, out);
        return;
    }
    throw InternalError("Unexpected plan node in EXPLAIN ANALYZE");
}

}  // namespace

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (context != nullptr && context->txn_ != nullptr && context->txn_->get_txn_mode()) {
        throw RMDBError("DDL statements are not allowed inside an explicit transaction");
    }
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                if (context->txn_->get_txn_mode()) {
                    throw RMDBError("Nested transactions are not supported");
                }
                txn_mgr_->start_explicit(context->txn_);
                break;
            }
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                break;
            }
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }
            case T_StaticCheckpoint:
            {
                if (recovery_manager_ == nullptr) throw InternalError("Recovery manager is not configured");
                recovery_manager_->create_static_checkpoint(
                    context->txn_ == nullptr ? INVALID_TXN_ID
                                             : context->txn_->get_transaction_id());
                break;
            }
            case T_SetIsolationSnapshot:
            {
                if (context->session_isolation_ == nullptr) throw InternalError("Missing session isolation state");
                *context->session_isolation_ = IsolationLevel::SNAPSHOT_ISOLATION;
                break;
            }
            case T_SetIsolationSerializable:
            {
                if (context->session_isolation_ == nullptr) throw InternalError("Missing session isolation state");
                *context->session_isolation_ = IsolationLevel::SERIALIZABLE;
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }

    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot,
                            std::vector<std::string> captions, Context *context) {
    // Materialize the complete result before emitting any output. SERIALIZABLE
    // dependency checks may abort while a scan is opened or advanced; in that
    // case the statement must return only "abort" instead of leaving a partial
    // header/result in output.txt.
    std::vector<std::vector<std::string>> result_rows;
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        columns.reserve(executorTreeRoot->cols().size());
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            char *rec_buf = tuple->data + col.offset;
            if (col.type == TYPE_INT) {
                col_str = std::to_string(*(int *)rec_buf);
            } else if (col.type == TYPE_FLOAT) {
                col_str = std::to_string(*(float *)rec_buf);
            } else if (col.type == TYPE_STRING) {
                col_str = std::string((char *)rec_buf, col.len);
                col_str.resize(strlen(col_str.c_str()));
            }
            columns.push_back(std::move(col_str));
        }
        result_rows.push_back(std::move(columns));
    }

    RecordPrinter rec_printer(captions.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);

    std::fstream outfile("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for (const auto &caption : captions) {
        outfile << " " << caption << " |";
    }
    outfile << "\n";

    for (const auto &columns : result_rows) {
        rec_printer.print_record(columns, context);
        outfile << "|";
        for (const auto &column : columns) {
            outfile << " " << column << " |";
        }
        outfile << "\n";
    }
    outfile.close();

    rec_printer.print_separator(context);
    RecordPrinter::print_record_count(result_rows.size(), context);
}


void QlManager::explain_analyze(
    std::unique_ptr<AbstractExecutor> executorTreeRoot,
    const std::shared_ptr<Plan> &plan, Context *context) {
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end();
         executorTreeRoot->nextTuple()) {
        (void)executorTreeRoot->Next();
    }

    std::ostringstream formatted;
    format_plan_tree(plan, 0, formatted);
    const std::string text = formatted.str();

    if (context->data_send_ != nullptr && context->offset_ != nullptr) {
        memcpy(context->data_send_ + *context->offset_, text.data(), text.size());
        *context->offset_ += static_cast<int>(text.size());
        context->data_send_[*context->offset_] = '\0';
    }

    std::fstream outfile("output.txt", std::ios::out | std::ios::app);
    outfile << text;
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
