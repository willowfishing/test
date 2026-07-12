#include "sm_manager.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) { throw DatabaseExistsError(db_name); }
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) { throw UnixError(); }
    if (chdir(db_name.c_str()) < 0) { throw UnixError(); }
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;
    std::ofstream ofs(DB_META_NAME);
    ofs << *new_db;
    delete new_db;
    disk_manager_->create_file(LOG_FILE_NAME);
    if (chdir("..") < 0) { throw UnixError(); }
}

void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) { throw DatabaseNotFoundError(db_name); }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) { throw UnixError(); }
}

void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) { throw DatabaseNotFoundError(db_name); }
    if (chdir(db_name.c_str()) < 0) { throw UnixError(); }
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) { if (chdir("..") < 0) { throw UnixError(); } throw DatabaseNotFoundError(db_name); }
    ifs >> db_;
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
        for (auto &index : tab.indexes) {
            ihs_.emplace(ix_manager_->get_index_name(tab.name, index.cols),
                         ix_manager_->open_index(tab.name, index.cols));
        }
    }
}

void SmManager::flush_meta() { std::ofstream ofs(DB_META_NAME); ofs << db_; }

void SmManager::close_db() {
    for (auto &entry : fhs_) { rm_manager_->close_file(entry.second.get()); }
    fhs_.clear();
    for (auto &entry : ihs_) { ix_manager_->close_index(entry.second.get()); }
    ihs_.clear();
    if (chdir("..") < 0) { throw UnixError(); }
}

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


void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    for (auto &index : tab.indexes) {
        std::string cols_str = "(";
        for (size_t i = 0; i < index.cols.size(); i++) {
            if (i > 0) cols_str += ",";
            cols_str += index.cols[i].name;
        }
        cols_str += ")";
        outfile << "| " << tab_name << " | unique | " << cols_str << " |\n";
    }
    outfile.close();
}

void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    printer.print_separator(context);
}

void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) { throw TableExistsError(tab_name); }
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {tab_name, col_def.name, col_def.type, col_def.len, curr_offset, false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    int record_size = curr_offset;
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    flush_meta();
}

void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) { throw TableNotFoundError(tab_name); }
    TabMeta &tab = db_.get_table(tab_name);
    if (fhs_.count(tab_name)) {
        rm_manager_->close_file(fhs_.at(tab_name).get());
        fhs_.erase(tab_name);
    }
    rm_manager_->destroy_file(tab_name);
    for (auto &index : tab.indexes) {
        std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
        if (ihs_.count(ix_name)) {
            ix_manager_->close_index(ihs_.at(ix_name).get());
            ihs_.erase(ix_name);
        }
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    db_.tabs_.erase(tab_name);
    flush_meta();
}

void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) { throw IndexExistsError(tab_name, col_names); }
    IndexMeta index;
    index.tab_name = tab_name;
    index.col_num = col_names.size();
    index.col_tot_len = 0;
    for (auto &col_name : col_names) {
        auto col_it = tab.get_col(col_name);
        index.cols.push_back(*col_it);
        index.col_tot_len += col_it->len;
    }
    ix_manager_->create_index(tab_name, index.cols);
    tab.indexes.push_back(index);
    ihs_.emplace(ix_manager_->get_index_name(tab_name, index.cols),
                 ix_manager_->open_index(tab_name, index.cols));
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    auto idx_it = tab.get_index_meta(col_names);
    std::string ix_name = ix_manager_->get_index_name(tab_name, idx_it->cols);
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_.at(ix_name).get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, idx_it->cols);
    tab.indexes.erase(idx_it);
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) { col_names.push_back(col.name); }
    drop_index(tab_name, col_names, context);
}
