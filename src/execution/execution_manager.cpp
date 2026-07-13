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

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#include "common/index_runtime.h"
#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_scan_cache.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "common/output_file.h"
#include "common/schema_change.h"
#include "index/ix.h"
#include "record_printer.h"
#include "recovery/log_manager.h"

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

namespace {

constexpr size_t kOutputColumnWidth = 16;

void append_fixed_width_cell(std::string *row, std::string_view cell) {
    row->append("| ");
    if (cell.size() > kOutputColumnWidth) {
        row->append(cell.data(), kOutputColumnWidth - 3);
        row->append("...");
    } else {
        row->append(kOutputColumnWidth - cell.size(), ' ');
        if (!cell.empty()) {
            row->append(cell.data(), cell.size());
        }
    }
    row->push_back(' ');
}

void append_output_file_cell(std::string *row, std::string_view cell) {
    row->append(" ");
    if (!cell.empty()) {
        row->append(cell.data(), cell.size());
    }
    row->append(" |");
}

size_t format_int_cell(int value, char *out) {
    char digits[16];
    size_t digit_count = 0;
    bool negative = value < 0;
    unsigned int magnitude = 0;
    if (negative) {
        magnitude = static_cast<unsigned int>(-(value + 1)) + 1;
    } else {
        magnitude = static_cast<unsigned int>(value);
    }
    do {
        digits[digit_count++] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    size_t pos = 0;
    if (negative) {
        out[pos++] = '-';
    }
    while (digit_count > 0) {
        out[pos++] = digits[--digit_count];
    }
    return pos;
}

void append_cell_to_rows(const char *rec_buf, const ColMeta &col, std::string *client_row,
                         std::string *file_row) {
    char number_buf[64];
    std::string_view cell;
    if (col.type == TYPE_INT) {
        size_t len = format_int_cell(*reinterpret_cast<const int *>(rec_buf), number_buf);
        cell = std::string_view(number_buf, len);
    } else if (col.type == TYPE_FLOAT) {
        int len = snprintf(number_buf, sizeof(number_buf), "%.6f", *reinterpret_cast<const float *>(rec_buf));
        cell = len <= 0 ? std::string_view()
                        : std::string_view(number_buf, std::min(static_cast<size_t>(len), sizeof(number_buf) - 1));
    } else if (col.type == TYPE_STRING) {
        const void *nul = std::memchr(rec_buf, '\0', static_cast<size_t>(col.len));
        size_t len = nul == nullptr ? static_cast<size_t>(col.len)
                                    : static_cast<const char *>(nul) - rec_buf;
        cell = std::string_view(rec_buf, len);
    } else {
        cell = std::string_view();
    }
    if (client_row != nullptr) {
        append_fixed_width_cell(client_row, cell);
    }
    if (file_row != nullptr) {
        append_output_file_cell(file_row, cell);
    }
}

void append_client_row(std::string *row, Context *context) {
    if (context->ellipsis_) {
        return;
    }
    row->append("|\n");
    if (*context->offset_ + RECORD_COUNT_LENGTH + row->length() < BUFFER_LENGTH) {
        memcpy(context->data_send_ + *(context->offset_), row->data(), row->length());
        *(context->offset_) += row->length();
    } else {
        context->ellipsis_ = true;
    }
}

std::string normalize_load_file_name(std::string file_name) {
    if (file_name.size() >= 2) {
        char first = file_name.front();
        char last = file_name.back();
        if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
            file_name = file_name.substr(1, file_name.size() - 2);
        }
    }
    return file_name;
}

std::vector<std::string> split_csv_line(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (char ch : line) {
        if (ch == '"') {
            in_quotes = !in_quotes;
        } else if (ch == ',' && !in_quotes) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

void split_csv_line_views(std::string *line, std::vector<std::string_view> *fields) {
    if (!line->empty() && line->back() == '\r') {
        line->pop_back();
    }
    fields->clear();
    bool in_quotes = false;
    size_t write_pos = 0;
    size_t field_start = 0;
    for (size_t read_pos = 0; read_pos < line->size(); ++read_pos) {
        char ch = (*line)[read_pos];
        if (ch == '"') {
            in_quotes = !in_quotes;
        } else if (ch == ',' && !in_quotes) {
            fields->emplace_back(line->data() + field_start, write_pos - field_start);
            field_start = write_pos;
        } else {
            (*line)[write_pos++] = ch;
        }
    }
    line->resize(write_pos);
    fields->emplace_back(line->data() + field_start, write_pos - field_start);
}

std::string_view trim_numeric_field(std::string_view field) {
    while (!field.empty() && std::isspace(static_cast<unsigned char>(field.front()))) {
        field.remove_prefix(1);
    }
    while (!field.empty() && std::isspace(static_cast<unsigned char>(field.back()))) {
        field.remove_suffix(1);
    }
    return field;
}

int parse_csv_int(std::string_view field) {
    field = trim_numeric_field(field);
    if (field.empty()) {
        throw RMDBError("invalid int value in load");
    }
    bool negative = false;
    size_t pos = 0;
    if (field[pos] == '+' || field[pos] == '-') {
        negative = field[pos] == '-';
        ++pos;
    }
    if (pos == field.size()) {
        throw RMDBError("invalid int value in load");
    }
    long long value = 0;
    for (; pos < field.size(); ++pos) {
        unsigned char ch = static_cast<unsigned char>(field[pos]);
        if (!std::isdigit(ch)) {
            throw RMDBError("invalid int value in load");
        }
        value = value * 10 + static_cast<int>(ch - '0');
        long long limit = negative ? -(static_cast<long long>(std::numeric_limits<int>::min()))
                                   : std::numeric_limits<int>::max();
        if (value > limit) {
            throw RMDBError("int value out of range in load");
        }
    }
    long long signed_value = negative ? -value : value;
    return static_cast<int>(signed_value);
}

Value csv_field_to_value(const std::string &field, ColType type) {
    Value value;
    if (type == TYPE_INT) {
        value.set_int(std::stoi(field));
    } else if (type == TYPE_FLOAT) {
        value.set_float(std::stof(field));
    } else if (type == TYPE_STRING) {
        value.set_str(field);
    } else {
        throw RMDBError("unsupported column type in load");
    }
    return value;
}

void write_csv_field_to_record(std::string_view field, const ColMeta &col, char *record, std::string *parse_scratch) {
    char *dst = record + col.offset;
    if (col.type == TYPE_INT) {
        int value = parse_csv_int(field);
        memcpy(dst, &value, sizeof(value));
    } else if (col.type == TYPE_FLOAT) {
        field = trim_numeric_field(field);
        parse_scratch->assign(field.data(), field.size());
        char *end = nullptr;
        errno = 0;
        float value = std::strtof(parse_scratch->c_str(), &end);
        if (end == parse_scratch->c_str() || *end != '\0' || errno == ERANGE) {
            throw RMDBError("invalid float value in load");
        }
        memcpy(dst, &value, sizeof(value));
    } else if (col.type == TYPE_STRING) {
        if (field.size() > static_cast<size_t>(col.len)) {
            throw StringOverflowError();
        }
        memset(dst, 0, static_cast<size_t>(col.len));
        if (!field.empty()) {
            memcpy(dst, field.data(), field.size());
        }
    } else {
        throw RMDBError("unsupported column type in load");
    }
}

size_t load_csv_rows_fast_no_index(RmFileHandle *fh, const TabMeta &tab, std::ifstream *input) {
    std::string line;
    std::vector<std::string_view> fields;
    fields.reserve(tab.cols.size());
    std::string parse_scratch;
    parse_scratch.reserve(64);
    BufferAccessStrategy cold_write_strategy(BufferAccessClass::ColdWrite);
    return fh->bulk_insert_records([&](char *record) -> bool {
        while (std::getline(*input, line)) {
            if (line.empty() || line == "\r") {
                continue;
            }
            split_csv_line_views(&line, &fields);
            if (fields.size() != tab.cols.size()) {
                throw RMDBError("csv column count mismatch for table " + tab.name);
            }
            for (size_t i = 0; i < fields.size(); ++i) {
                write_csv_field_to_record(fields[i], tab.cols[i], record, &parse_scratch);
            }
            return true;
        }
        return false;
    }, &cold_write_strategy);
}

void load_csv_rows(SmManager *sm_manager, const std::string &tab_name, const std::string &file_name, Context *context) {
    if (!sm_manager->db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    std::string normalized_file_name = normalize_load_file_name(file_name);
    std::ifstream input(normalized_file_name);
    if (!input.is_open()) {
        throw RMDBError("failed to open csv file: " + normalized_file_name);
    }

    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    auto fh = sm_manager->fhs_.at(tab_name).get();
    Context load_context(nullptr, nullptr, nullptr, nullptr);
    std::string line;
    std::getline(input, line);  // header
    if (tab.indexes.empty()) {
        load_csv_rows_fast_no_index(fh, tab, &input);
        std::vector<std::string> changed_cols;
        changed_cols.reserve(tab.cols.size());
        for (const auto &col : tab.cols) {
            changed_cols.push_back(col.name);
        }
        rmdb::bump_scan_cache_columns(tab_name, changed_cols);
        fh->flush();
        return;
    }
    while (std::getline(input, line)) {
        if (line.empty() || line == "\r") {
            continue;
        }
        auto fields = split_csv_line(line);
        if (fields.size() != tab.cols.size()) {
            throw RMDBError("csv column count mismatch for table " + tab_name);
        }
        std::vector<Value> values;
        values.reserve(fields.size());
        for (size_t i = 0; i < fields.size(); ++i) {
            values.push_back(csv_field_to_value(fields[i], tab.cols[i].type));
        }
        // Load is trusted bulk import before benchmark transactions.  Reusing the session
        // txn here makes every row pay SI conflict checks, write-set bookkeeping and WAL.
        // A log-free local context keeps heap/index correctness and flushes once below.
        InsertExecutor executor(sm_manager, tab_name, std::move(values), &load_context);
        executor.Next();
    }
    std::vector<std::string> changed_cols;
    changed_cols.reserve(tab.cols.size());
    for (const auto &col : tab.cols) {
        changed_cols.push_back(col.name);
    }
    rmdb::bump_scan_cache_columns(tab_name, changed_cols);
    fh->flush();
    auto index_bindings = rmdb::bind_table_indexes(sm_manager, tab_name, tab);
    for (const auto &binding : index_bindings) {
        binding.ih->flush();
    }
}

}  // namespace

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                rmdb::invalidate_sql_template_caches();
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                rmdb::invalidate_sql_template_caches();
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                rmdb::invalidate_sql_template_caches();
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                rmdb::invalidate_sql_template_caches();
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        if (x->tag == T_Load) {
            load_csv_rows(sm_manager_, x->tab_name_, x->file_name_, context);
            rmdb::invalidate_sql_template_caches();
            return;
        }
        throw InternalError("Unexpected dml type in run_mutli_query");
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto set_isolation = std::dynamic_pointer_cast<SetTransactionIsolationPlan>(plan)) {
        if (context != nullptr && context->session_isolation_ != nullptr) {
            *(context->session_isolation_) = set_isolation->isolation_level_;
        }
        return;
    }
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
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                if (context->txn_ != nullptr) {
                    context->txn_->set_txn_mode(true);
                    *txn_id = context->txn_->get_transaction_id();
                }
                break;
            }  
            case T_Transaction_commit:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->commit(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_rollback:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->abort(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_abort:
            {
                if (context->txn_ != nullptr) {
                    txn_mgr_->abort(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                }
                *txn_id = INVALID_TXN_ID;
                break;
            }
            case T_Checkpoint:
            {
                // 1. Capture active transactions and their latest log positions.
                Transaction *exclude_txn =
                    context != nullptr && context->txn_ != nullptr && !context->txn_->get_txn_mode() ? context->txn_ : nullptr;
                auto active_txns = txn_mgr_->CollectActiveTxnCheckpointInfo(exclude_txn);
                // 2. Flush pending log buffer
                if (context->log_mgr_ != nullptr) {
                    context->log_mgr_->flush_log_to_disk();
                }
                // 3. Write checkpoint log record
                CheckpointLogRecord ckpt_record(active_txns);
                lsn_t ckpt_lsn = INVALID_LSN;
                if (context->log_mgr_ != nullptr) {
                    ckpt_lsn = context->log_mgr_->add_log_to_buffer(&ckpt_record);
                    context->log_mgr_->flush_log_to_disk();
                }
                // 4. Flush all data pages to disk
                for (auto &entry : sm_manager_->fhs_) {
                    entry.second->flush();
                }
                for (auto &entry : sm_manager_->ihs_) {
                    entry.second->flush();
                }
                // 5. Write restart file with checkpoint LSN
                if (ckpt_lsn != INVALID_LSN) {
                    std::ofstream chkpt_file("db.chkpt", std::ios::trunc);
                    chkpt_file << ckpt_lsn;
                    chkpt_file.close();
                }
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }

    } else if(auto x = std::dynamic_pointer_cast<ExplainPlan>(plan)) {
        for (auto &line : x->lines_) {
            std::string out = line + "\n";
            memcpy(context->data_send_ + *(context->offset_), out.c_str(), out.length());
            *(context->offset_) += out.length();
            rmdb::append_output_file(out);
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
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            Context *context) {
    executorTreeRoot->beginTuple();

    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.col_name);
    }

    // Print header into buffer
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file
    if (rmdb::is_output_file_enabled()) {
        std::string output = "|";
        for(size_t i = 0; i < captions.size(); ++i) {
            output += " " + captions[i] + " |";
        }
        output += "\n";
        rmdb::append_output_file(output);
    }

    std::vector<ColMeta> output_cols;
    std::vector<size_t> output_col_idxs;
    output_cols.reserve(sel_cols.size());
    output_col_idxs.reserve(sel_cols.size());
    const auto &root_cols = executorTreeRoot->cols();
    for (const auto &sel_col : sel_cols) {
        auto pos = executorTreeRoot->get_col(root_cols, sel_col);
        output_cols.push_back(*pos);
        output_col_idxs.push_back(static_cast<size_t>(pos - root_cols.begin()));
        const auto &col = output_cols.back();
        if (col.offset < 0 || col.len < 0 ||
            static_cast<size_t>(col.offset + col.len) > executorTreeRoot->tupleLen()) {
            throw RMDBError("invalid output column offset");
        }
    }

    // Print records
    size_t num_rec = 0;
    const bool write_output_file = rmdb::is_output_file_enabled();
    // 执行query_plan
    for (; !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto tuple = executorTreeRoot->ReadTupleView();
        if (!tuple) {
            break;
        }
        std::string client_row;
        client_row.reserve(output_cols.size() * (kOutputColumnWidth + 3) + 2);
        std::string file_row;
        if (write_output_file) {
            file_row.reserve(output_cols.size() * (kOutputColumnWidth + 3) + 2);
            file_row.push_back('|');
        }
        for (size_t i = 0; i < output_cols.size(); ++i) {
            const auto &col = output_cols[i];
            append_cell_to_rows(tuple.view->cell_at(col, output_col_idxs[i]), col, &client_row,
                                write_output_file ? &file_row : nullptr);
        }
        append_client_row(&client_row, context);
        if (write_output_file) {
            file_row.push_back('\n');
            rmdb::append_output_file(file_row);
        }
        num_rec++;
    }
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
