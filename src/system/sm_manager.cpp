/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "execution/index_key_utils.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
#include "transaction/transaction_manager.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    // 进入数据库目录
    if (chdir(db_name.c_str()) < 0) {
        throw DatabaseNotFoundError(db_name);
    }
    // 从磁盘中读取元数据文件 db.meta
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw DatabaseNotFoundError(db_name);
    }
    ifs >> db_;
    ifs.close();
    // 打开数据库中每张表的记录文件
    for (auto &entry : db_.tabs_) {
        const std::string &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    }
    // 打开所有索引文件
    for (auto &tab_entry : db_.tabs_) {
        auto &tab = tab_entry.second;
        for (auto &index : tab.indexes) {
            std::string ix_name = ix_manager_->get_index_name(tab.name, index.cols);
            ihs_.emplace(ix_name, ix_manager_->open_index(tab.name, index.cols));
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}


void SmManager::flush_all_pages() {
    // Persist file headers before the checkpoint restart pointer is published.
    for (auto &entry : fhs_) {
        entry.second->flush_file_hdr();
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
        disk_manager_->sync_file(entry.second->GetFd());
    }
    for (auto &entry : ihs_) {
        ix_manager_->flush_index(entry.second.get());
    }
    flush_meta();
    int meta_fd = ::open(DB_META_NAME.c_str(), O_RDONLY);
    if (meta_fd < 0) throw UnixError();
    if (::fsync(meta_fd) != 0) {
        const int saved_errno = errno;
        ::close(meta_fd);
        errno = saved_errno;
        throw UnixError();
    }
    if (::close(meta_fd) != 0) throw UnixError();
}

void SmManager::reset_all_indexes() {
    struct IndexToReset {
        std::string table;
        IndexMeta meta;
    };
    std::vector<IndexToReset> indexes;
    for (const auto &[table_name, tab] : db_.tabs_) {
        for (const auto &index : tab.indexes) indexes.push_back({table_name, index});
    }

    for (const auto &item : indexes) {
        const std::string index_name = ix_manager_->get_index_name(item.table, item.meta.cols);
        auto existing = ihs_.find(index_name);
        if (existing != ihs_.end()) {
            ix_manager_->close_index(existing->second.get());
            ihs_.erase(existing);
        }
        if (ix_manager_->exists(item.table, item.meta.cols)) {
            ix_manager_->destroy_index(item.table, item.meta.cols);
        }
        ix_manager_->create_index(item.table, item.meta.cols);
        ihs_.emplace(index_name, ix_manager_->open_index(item.table, item.meta.cols));
    }
}

void SmManager::rebuild_all_indexes(TransactionManager *txn_manager) {
    struct IndexToBuild {
        std::string table;
        IndexMeta meta;
    };
    std::vector<IndexToBuild> indexes;
    for (const auto &[table_name, tab] : db_.tabs_) {
        for (const auto &index : tab.indexes) indexes.push_back({table_name, index});
    }

    for (const auto &item : indexes) {
        const std::string index_name = ix_manager_->get_index_name(item.table, item.meta.cols);
        auto existing = ihs_.find(index_name);
        if (existing != ihs_.end()) {
            ix_manager_->close_index(existing->second.get());
            ihs_.erase(existing);
        }
        if (ix_manager_->exists(item.table, item.meta.cols)) {
            ix_manager_->destroy_index(item.table, item.meta.cols);
        }
        ix_manager_->create_index(item.table, item.meta.cols);
        auto handle = ix_manager_->open_index(item.table, item.meta.cols);

        RmFileHandle *fh = fhs_.at(item.table).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            const Rid rid = scan.rid();
            if (txn_manager != nullptr && txn_manager->is_committed_deleted(item.table, rid)) {
                continue;
            }
            auto record = fh->get_record(rid, nullptr);
            auto key = make_index_key(item.meta, record->data);
            if (handle->insert_entry(key.data(), rid, nullptr) == IX_NO_PAGE) {
                throw UniqueConstraintError(item.table);
            }
        }
        ix_manager_->flush_index(handle.get());
        ihs_.emplace(index_name, std::move(handle));
    }
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 关闭所有表的文件句柄
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    // 关闭所有索引文件
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    // 刷新元数据
    flush_meta();
    // 返回上层目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    // 关闭并删除表的数据文件
    auto fh_it = fhs_.find(tab_name);
    if (fh_it != fhs_.end()) {
        rm_manager_->close_file(fh_it->second.get());
        fhs_.erase(fh_it);
    }
    rm_manager_->destroy_file(tab_name);
    // 删除相关索引
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        std::string index_name = ix_manager_->get_index_name(tab_name, index.cols);
        auto ih_it = ihs_.find(index_name);
        if (ih_it != ihs_.end()) {
            ix_manager_->close_index(ih_it->second.get());
            ihs_.erase(ih_it);
        }
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    // 从元数据中移除
    db_.tabs_.erase(tab_name);
    // 刷新元数据
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string &tab_name, const std::vector<std::string> &col_names,
                             Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    if (col_names.empty()) {
        throw InternalError("An index must contain at least one column");
    }

    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    std::vector<ColMeta> index_cols;
    int col_tot_len = 0;
    for (const auto &col_name : col_names) {
        auto col_it = tab.get_col(col_name);
        index_cols.push_back(*col_it);
        col_tot_len += col_it->len;
    }
    if (col_tot_len > IX_MAX_COL_LEN) {
        throw InvalidColLengthError(col_tot_len);
    }

    const std::string ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    std::unique_ptr<IxIndexHandle> ih;
    bool created = false;
    try {
        ix_manager_->create_index(tab_name, index_cols);
        created = true;
        ih = ix_manager_->open_index(tab_name, index_cols);

        RmFileHandle *fh = fhs_.at(tab_name).get();
        Transaction *txn = context == nullptr ? nullptr : context->txn_;
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            const Rid rid = scan.rid();
            auto rec = fh->get_record(rid, context);
            std::vector<char> key(col_tot_len);
            int offset = 0;
            for (const auto &col : index_cols) {
                memcpy(key.data() + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            if (ih->insert_entry(key.data(), rid, txn) == IX_NO_PAGE) {
                throw UniqueConstraintError(tab_name);
            }
        }
    } catch (...) {
        if (ih != nullptr) {
            ix_manager_->close_index(ih.get());
            ih.reset();
        }
        if (created && ix_manager_->exists(tab_name, index_cols)) {
            ix_manager_->destroy_index(tab_name, index_cols);
        }
        throw;
    }

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_tot_len = col_tot_len;
    index_meta.col_num = static_cast<int>(index_cols.size());
    index_meta.cols = index_cols;
    tab.indexes.push_back(index_meta);
    ihs_.emplace(ix_name, std::move(ih));
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    if (!tab.is_index(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    // 关闭并删除索引文件
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
        ix_manager_->close_index(ih_it->second.get());
        ihs_.erase(ih_it);
    }
    ix_manager_->destroy_index(tab_name, col_names);
    // 从表元数据中移除
    auto idx_it = tab.get_index_meta(col_names);
    tab.indexes.erase(idx_it);
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {

}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    RecordPrinter printer(3);
    printer.print_separator(context);
    printer.print_record({"table_name", "unique", "column_name"}, context);
    printer.print_separator(context);
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    for (auto &index : tab.indexes) {
        std::string cols_str = "(";
        for (size_t i = 0; i < index.cols.size(); i++) {
            if (i > 0) cols_str += ",";
            cols_str += index.cols[i].name;
        }
        cols_str += ")";
        printer.print_record({tab_name, "unique", cols_str}, context);
        outfile << "| " << tab_name << " | unique | " << cols_str << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}
