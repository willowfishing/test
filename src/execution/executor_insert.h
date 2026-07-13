/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        // 1. 构造记录
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (col.type != val.type) {
                if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                    float fval = static_cast<float>(val.int_val);
                    val.set_float(fval);
                } else {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }

        // 2. DELETE-INSERT conflict check: scan table for pending deletes
        if (context_ && context_->txn_ && context_->txn_mgr_) {
            // If table has indexes, use indexed check
            if (!tab_.indexes.empty()) {
                auto& index = tab_.indexes[0];
                char* key = new char[index.col_tot_len];
                int offset = 0;
                for(size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                if (context_->txn_mgr_->HasPendingDelete(fh_->GetFd(),
                        std::string(key, index.col_tot_len), context_->txn_->get_transaction_id())) {
                    delete[] key;
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
                delete[] key;
            }
            // Also scan table for any in-progress delete on matching record values
            RmScan scan(fh_);
            while (!scan.is_end()) {
                Rid scan_rid = scan.rid();
                TupleMeta meta = fh_->get_meta(scan_rid);
                if (meta.is_deleted_ && meta.writer_txn_ != INVALID_TXN_ID &&
                    meta.writer_txn_ != context_->txn_->get_transaction_id()) {
                    auto *deleter = context_->txn_mgr_->get_transaction_safe(meta.writer_txn_);
                    if (deleter && deleter->get_state() != TransactionState::COMMITTED &&
                        deleter->get_state() != TransactionState::ABORTED) {
                        // Check if the deleted record matches our new record's values
                        auto del_rec = fh_->get_record(scan_rid, context_);
                        bool matches = true;
                        for (size_t c = 0; c < tab_.cols.size(); c++) {
                            int off = tab_.cols[c].offset;
                            int len = tab_.cols[c].len;
                            if (memcmp(rec.data + off, del_rec->data + off, len) != 0) {
                                matches = false; break;
                            }
                        }
                        if (matches) {
                            throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                AbortReason::DEADLOCK_PREVENTION);
                        }
                    }
                }
                scan.next();
            }
        }

        // 3. 对所有索引做唯一性检查（在插入表记录之前）
        for(size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto& index = tab_.indexes[i];
            std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            if (!sm_manager_->ihs_.count(ix_name)) continue;
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            char* key = new char[index.col_tot_len];
            int offset = 0;
            for(size_t j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            std::vector<Rid> result;
            if (ih->get_value(key, &result, context_->txn_) && !result.empty()) {
                for (auto &existing_rid : result) {
                    if (context_ && context_->txn_ && context_->txn_mgr_) {
                        TupleMeta meta = fh_->get_meta(existing_rid);
                        if (meta.writer_txn_ != INVALID_TXN_ID &&
                            meta.writer_txn_ != context_->txn_->get_transaction_id()) {
                            auto *writer = context_->txn_mgr_->get_transaction_safe(meta.writer_txn_);
                            if (writer && writer->get_state() != TransactionState::COMMITTED &&
                                writer->get_state() != TransactionState::ABORTED) {
                                delete[] key;
                                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                    AbortReason::DEADLOCK_PREVENTION);
                            }
                        }
                    }
                }
                delete[] key;
                throw IncompatibleTypeError("Duplicate key in unique index", "");
            }
            delete[] key;
        }

        // 3. 所有检查通过，插入表记录
        rid_ = fh_->insert_record(rec.data, context_);

        // 4. 记录写操作到事务，SSI依赖跟踪
        if (context_ != nullptr && context_->txn_ != nullptr) {
            WriteRecord *wr = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_);
            context_->txn_->append_write_record(wr);
            if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                context_->txn_->add_write_table(tab_name_);
                // Check if any other SER txn has a predicate read on this table
                context_->txn_mgr_->CheckAndAddPredicateDep(tab_name_, context_->txn_);
            }
        }

        // 5. 插入所有索引
        for(size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto& index = tab_.indexes[i];
            std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            if (!sm_manager_->ihs_.count(ix_name)) continue;
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            char* key = new char[index.col_tot_len];
            int offset = 0;
            for(size_t j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            ih->insert_entry(key, rid_, context_->txn_);
            delete[] key;
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};