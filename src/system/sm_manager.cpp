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

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record/rm_scan.h"
#include "record_printer.h"
#include "common/index_runtime.h"
#include "common/output_file.h"
#include "common/schema_change.h"
#include "execution/executor_scan_cache.h"

namespace {

void collect_index_entries(RmFileHandle *fh, const IndexMeta &index,
                           std::vector<std::pair<std::string, Rid>> *entries) {
    std::string key(index.col_tot_len, '\0');
    for (RmScan scan(fh, BufferAccessClass::IndexBuild); !scan.is_end(); scan.next()) {
        scan.with_current_slot([&](const char *slot) {
            rmdb::build_index_key_into(index, slot, scan.rid(), &key);
            entries->emplace_back(key, scan.rid());
            return true;
        });
    }
}

}  // namespace

int SmManager::hidden_index_count() const {
    int count = 0;
    for (const auto &entry : db_.tabs_) {
        for (const auto &index : entry.second.indexes) {
            if (index.hidden) {
                count++;
            }
        }
    }
    return count;
}

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
    std::ofstream clean_marker(DB_CLEAN_SHUTDOWN_MARKER, std::ios::trunc);
    clean_marker.close();

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
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    bool clean_shutdown = disk_manager_->is_file(DB_CLEAN_SHUTDOWN_MARKER);
    if (clean_shutdown) {
        if (unlink(DB_CLEAN_SHUTDOWN_MARKER.c_str()) < 0) {
            throw UnixError();
        }
    }
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;
    for (auto &entry : db_.tabs_) {
        fhs_.emplace(entry.first, rm_manager_->open_file(entry.first));
        if (!clean_shutdown) {
            fhs_.at(entry.first)->rebuild_file_hdr_from_disk();
        }
        for (auto &index : entry.second.indexes) {
            if (index.index_name.empty()) {
                index.index_name = ix_manager_->get_index_name(entry.first, index.cols);
            }
            if (!clean_shutdown) {
                ix_manager_->destroy_index(entry.first, index);
                ix_manager_->create_index(entry.first, index);
            }
            auto ih = ix_manager_->open_index(entry.first, index);
            if (!clean_shutdown) {
                std::vector<std::pair<std::string, Rid>> entries;
                collect_index_entries(fhs_.at(entry.first).get(), index, &entries);
                ih->bulk_load(entries, index.unique);
            }
            ihs_.emplace(ix_manager_->get_index_name(entry.first, index), std::move(ih));
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

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    flush_meta();
    int meta_fd = disk_manager_->get_file_fd(DB_META_NAME);
    disk_manager_->sync_file(meta_fd);
    disk_manager_->close_file(meta_fd);
    // After a clean shutdown, all dirty pages have been flushed to disk, so the
    // WAL is no longer needed. Truncating it here avoids two problems on the
    // next startup:
    //   1) Recovery::analyze() reading the entire stale log into memory (OOM).
    //   2) Redo/undo doing unnecessary I/O on already-applied records.
    disk_manager_->truncate_log();
    disk_manager_->sync_log();
    disk_manager_->remove_file_if_exists(CHECKPOINT_FILE_NAME);
    std::ofstream clean_marker(DB_CLEAN_SHUTDOWN_MARKER, std::ios::trunc);
    clean_marker.close();
    int marker_fd = disk_manager_->get_file_fd(DB_CLEAN_SHUTDOWN_MARKER);
    disk_manager_->sync_file(marker_fd);
    disk_manager_->close_file(marker_fd);
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    rmdb::append_output_file("| Tables |\n");
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        rmdb::append_output_file("| " + tab.name + " |\n");
    }
    printer.print_separator(context);
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

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        if (index.hidden) {
            continue;
        }
        std::string output = "| " + tab_name + " | " + (index.unique ? "unique" : "non_unique") + " | (";
        for (int i = 0; i < index.col_num; ++i) {
            if (i > 0) {
                output += ",";
            }
            output += index.cols[i].name;
        }
        output += ") |\n";
        rmdb::append_output_file(output);
    }
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
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    auto &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        std::string ix_name = ix_manager_->get_index_name(tab_name, index);
        if (ihs_.count(ix_name)) {
            ix_manager_->close_index(ihs_.at(ix_name).get());
            ihs_.erase(ix_name);
        }
        ix_manager_->destroy_index(tab_name, index);
    }
    if (fhs_.count(tab_name)) {
        rm_manager_->close_file(fhs_.at(tab_name).get());
        fhs_.erase(tab_name);
    }
    rm_manager_->destroy_file(tab_name);
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }
    std::vector<ColMeta> index_cols;
    int col_tot_len = 0;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
        col_tot_len += col->len;
        col->index = true;
    }
    std::unique_ptr<IxIndexHandle> ih;
    bool index_file_created = false;
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.index_name = ix_manager_->get_index_name(tab_name, index_cols);
    index_meta.col_tot_len = col_tot_len;
    index_meta.col_num = static_cast<int>(index_cols.size());
    index_meta.cols = index_cols;
    index_meta.unique = true;
    index_meta.hidden = false;
    try {
        ix_manager_->create_index(tab_name, index_meta);
        index_file_created = true;
        ih = ix_manager_->open_index(tab_name, index_meta);

        // Bulk-load: collect, sort, build tree bottom-up
        std::vector<std::pair<std::string, Rid>> entries;
        collect_index_entries(fhs_.at(tab_name).get(), index_meta, &entries);
        ih->bulk_load(entries, index_meta.unique);

        tab.indexes.push_back(index_meta);
        ihs_.emplace(ix_manager_->get_index_name(tab_name, index_meta), std::move(ih));
        flush_meta();
    } catch (...) {
        if (ih != nullptr) {
            ix_manager_->close_index(ih.get());
            ih.reset();
        }
        for (auto &col_name : col_names) {
            tab.get_col(col_name)->index = false;
        }
        if (index_file_created && ix_manager_->exists(tab_name, index_meta)) {
            ix_manager_->destroy_index(tab_name, index_meta);
        }
        throw;
    }
}

bool SmManager::create_internal_non_unique_index(const std::string& tab_name,
                                                 const std::vector<std::string>& col_names, Context* context) {
    (void)context;
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        if (index.hidden && !index.unique && index.matches_cols(col_names)) {
            return false;
        }
    }

    std::vector<ColMeta> index_cols;
    int logical_col_tot_len = 0;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
        logical_col_tot_len += col->len;
    }

    std::string name_base = tab_name + "__internal_non_unique";
    for (const auto &col_name : col_names) {
        name_base += "_" + col_name;
    }
    std::string index_name = name_base + ".idx";
    int suffix = 1;
    while (disk_manager_->is_file(index_name)) {
        index_name = name_base + "_" + std::to_string(suffix++) + ".idx";
    }

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.index_name = index_name;
    index_meta.col_tot_len = logical_col_tot_len + static_cast<int>(sizeof(Rid));
    index_meta.col_num = static_cast<int>(index_cols.size());
    index_meta.cols = index_cols;
    index_meta.unique = false;
    index_meta.hidden = true;

    std::unique_ptr<IxIndexHandle> ih;
    bool index_file_created = false;
    try {
        ix_manager_->create_index(tab_name, index_meta);
        index_file_created = true;
        ih = ix_manager_->open_index(tab_name, index_meta);

        std::vector<std::pair<std::string, Rid>> entries;
        collect_index_entries(fhs_.at(tab_name).get(), index_meta, &entries);
        ih->bulk_load(entries, index_meta.unique);

        tab.indexes.push_back(index_meta);
        try {
            ihs_.emplace(ix_manager_->get_index_name(tab_name, index_meta), std::move(ih));
            flush_meta();
            rmdb::invalidate_sql_template_caches();
            rmdb::bump_scan_cache_columns(tab_name, col_names);
        } catch (...) {
            auto inserted_it = tab.get_index_meta(col_names, true);
            tab.indexes.erase(inserted_it);
            std::string ix_name = ix_manager_->get_index_name(tab_name, index_meta);
            auto ih_it = ihs_.find(ix_name);
            if (ih_it != ihs_.end()) {
                ix_manager_->close_index(ih_it->second.get());
                ihs_.erase(ih_it);
            }
            if (ix_manager_->exists(tab_name, index_meta)) {
                ix_manager_->destroy_index(tab_name, index_meta);
            }
            throw;
        }
        return true;
    } catch (...) {
        if (ih != nullptr) {
            ix_manager_->close_index(ih.get());
            ih.reset();
        }
        std::string ix_name = ix_manager_->get_index_name(tab_name, index_meta);
        auto ih_it = ihs_.find(ix_name);
        if (ih_it != ihs_.end()) {
            ix_manager_->close_index(ih_it->second.get());
            ihs_.erase(ih_it);
        }
        if (index_file_created && ix_manager_->exists(tab_name, index_meta)) {
            ix_manager_->destroy_index(tab_name, index_meta);
        }
        throw;
    }
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
    auto index_it = tab.get_index_meta(col_names);
    for (auto &col_meta : index_it->cols) {
        tab.get_col(col_meta.name)->index = false;
    }
    std::string ix_name = ix_manager_->get_index_name(tab_name, *index_it);
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_.at(ix_name).get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, *index_it);
    tab.indexes.erase(index_it);
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}
