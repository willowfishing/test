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
#include <fstream>
#include "common/index_runtime.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "execution_common.h"
#include "executor_abstract.h"
#include "executor_scan_cache.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    struct CompiledSetClause {
        int lhs_offset = 0;
        int rhs_offset = 0;
        int len = 0;
        ColType lhs_type = TYPE_INT;
        ColType rhs_type = TYPE_INT;
        SetOp op = SetOp::ASSIGN;
        const char *literal = nullptr;
        void (*apply)(char *, const char *, const CompiledSetClause &) = nullptr;
    };

    static void apply_literal_assign(char *dst, const char *, const CompiledSetClause &clause) {
        memcpy(dst + clause.lhs_offset, clause.literal, clause.len);
    }

    static void apply_column_assign(char *dst, const char *src, const CompiledSetClause &clause) {
        memcpy(dst + clause.lhs_offset, src + clause.rhs_offset, clause.len);
    }

    static void apply_column_assign_int_to_float(char *dst, const char *src, const CompiledSetClause &clause) {
        float value = static_cast<float>(*reinterpret_cast<const int *>(src + clause.rhs_offset));
        memcpy(dst + clause.lhs_offset, &value, clause.len);
    }

    static void apply_int_arithmetic(char *dst, const char *src, const CompiledSetClause &clause) {
        int base = *reinterpret_cast<const int *>(src + clause.rhs_offset);
        int delta = *reinterpret_cast<const int *>(clause.literal);
        int result = base;
        if (clause.op == SetOp::ADD) {
            result = base + delta;
        } else if (clause.op == SetOp::SUB) {
            result = base - delta;
        } else if (clause.op == SetOp::MUL) {
            result = base * delta;
        } else if (clause.op == SetOp::DIV) {
            if (delta == 0) {
                throw RMDBError("Division by zero");
            }
            result = base / delta;
        }
        memcpy(dst + clause.lhs_offset, &result, clause.len);
    }

    static void apply_float_arithmetic(char *dst, const char *src, const CompiledSetClause &clause) {
        float base = clause.rhs_type == TYPE_FLOAT
                         ? *reinterpret_cast<const float *>(src + clause.rhs_offset)
                         : static_cast<float>(*reinterpret_cast<const int *>(src + clause.rhs_offset));
        float delta = *reinterpret_cast<const float *>(clause.literal);
        float result = base;
        if (clause.op == SetOp::ADD) {
            result = base + delta;
        } else if (clause.op == SetOp::SUB) {
            result = base - delta;
        } else if (clause.op == SetOp::MUL) {
            result = base * delta;
        } else if (clause.op == SetOp::DIV) {
            if (delta == 0.0f) {
                throw RMDBError("Division by zero");
            }
            result = base / delta;
        }
        memcpy(dst + clause.lhs_offset, &result, clause.len);
    }

    static void apply_unsupported_arithmetic(char *, const char *, const CompiledSetClause &clause) {
        throw IncompatibleTypeError("numeric column", coltype2str(clause.lhs_type));
    }

    const TabMeta *tab_ = nullptr;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    std::vector<rmdb::IndexBinding> index_bindings_;
    std::vector<std::string> index_key_scratch_;
    std::vector<std::string> index_key_scratch_alt_;
    std::vector<std::string> changed_cols_;
    std::vector<bool> index_key_touched_;
    std::vector<CompiledSetClause> compiled_set_clauses_;
    bool update_touches_index_columns_{false};
    bool key_conflict_check_required_{false};
    bool refresh_committed_hot_update_{false};
    struct InsertedIndexKey {
        IxIndexHandle *ih;
        std::string key;
    };

    void compile_set_clauses() {
        compiled_set_clauses_.clear();
        compiled_set_clauses_.reserve(set_clauses_.size());
        refresh_committed_hot_update_ = !set_clauses_.empty() && !update_touches_index_columns_;
        for (auto &set_clause : set_clauses_) {
            auto lhs_col = tab_->get_col(set_clause.lhs.col_name);
            CompiledSetClause compiled;
            compiled.lhs_offset = lhs_col->offset;
            compiled.len = lhs_col->len;
            compiled.lhs_type = lhs_col->type;
            compiled.op = set_clause.op;

            if (!set_clause.rhs_is_col) {
                if (set_clause.rhs.raw == nullptr) {
                    set_clause.rhs.init_raw(lhs_col->len);
                }
                compiled.literal = set_clause.rhs.raw->data;
                compiled.apply = apply_literal_assign;
                refresh_committed_hot_update_ = false;
                compiled_set_clauses_.push_back(compiled);
                continue;
            }

            auto rhs_col = tab_->get_col(set_clause.rhs_col.col_name);
            compiled.rhs_offset = rhs_col->offset;
            compiled.rhs_type = rhs_col->type;

            if (set_clause.op == SetOp::ASSIGN) {
                compiled.apply = lhs_col->type == TYPE_FLOAT && rhs_col->type == TYPE_INT
                                     ? apply_column_assign_int_to_float
                                     : apply_column_assign;
                refresh_committed_hot_update_ = false;
                compiled_set_clauses_.push_back(compiled);
                continue;
            }

            if (set_clause.rhs.raw == nullptr) {
                set_clause.rhs.init_raw(lhs_col->len);
            }
            compiled.literal = set_clause.rhs.raw->data;
            if (lhs_col->type == TYPE_INT) {
                compiled.apply = apply_int_arithmetic;
            } else if (lhs_col->type == TYPE_FLOAT) {
                compiled.apply = apply_float_arithmetic;
            } else {
                compiled.apply = apply_unsupported_arithmetic;
            }
            if (set_clause.lhs.col_name != set_clause.rhs_col.col_name) {
                refresh_committed_hot_update_ = false;
            }
            compiled_set_clauses_.push_back(compiled);
        }
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context,
                   std::shared_ptr<const PlanRuntimeCache> runtime_cache = nullptr) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        const bool use_cache = runtime_cache != nullptr && runtime_cache->has_table;
        tab_ = use_cache ? runtime_cache->tab : &sm_manager_->db_.get_table(tab_name);
        fh_ = use_cache ? runtime_cache->fh : sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
        index_bindings_ = runtime_cache != nullptr && runtime_cache->has_index_bindings
                              ? runtime_cache->index_bindings
                              : rmdb::bind_table_indexes(sm_manager_, tab_name_, *tab_);
        index_key_scratch_.resize(index_bindings_.size());
        index_key_scratch_alt_.resize(index_bindings_.size());
        index_key_touched_.assign(index_bindings_.size(), false);
        for (size_t i = 0; i < index_bindings_.size(); ++i) {
            index_key_scratch_[i].resize(index_bindings_[i].meta->col_tot_len);
            index_key_scratch_alt_[i].resize(index_bindings_[i].meta->col_tot_len);
        }
        changed_cols_.reserve(set_clauses_.size());
        for (const auto &set_clause : set_clauses_) {
            changed_cols_.push_back(set_clause.lhs.col_name);
        }
        for (const auto &col_name : changed_cols_) {
            if (col_name == "id") {
                key_conflict_check_required_ = true;
            }
            for (size_t i = 0; i < index_bindings_.size(); ++i) {
                const auto &index = *index_bindings_[i].meta;
                for (const auto &index_col : index.cols) {
                    if (index_col.name == col_name) {
                        index_key_touched_[i] = true;
                        update_touches_index_columns_ = true;
                        key_conflict_check_required_ = key_conflict_check_required_ || index.unique;
                        break;
                    }
                }
            }
        }
        compile_set_clauses();
    }
    std::unique_ptr<RmRecord> Next() override {
        std::vector<Rid> effective_rids;
        std::vector<std::unique_ptr<RmRecord>> old_records;
        std::vector<std::unique_ptr<RmRecord>> new_records;
        std::vector<TupleMeta> old_metas;
        old_records.reserve(rids_.size());
        new_records.reserve(rids_.size());
        old_metas.reserve(rids_.size());
        effective_rids.reserve(rids_.size());
        auto is_self_arithmetic_set_clause = [&](const SetClause &set_clause) {
            return set_clause.rhs_is_col && set_clause.lhs.col_name == set_clause.rhs_col.col_name &&
                   set_clause.op != SetOp::ASSIGN;
        };
        auto can_rebase_literal_assign = [&](const SetClause &set_clause, const Rid &rid) {
            if (set_clause.rhs_is_col || set_clause.op != SetOp::ASSIGN) {
                return false;
            }
            auto lhs_col = tab_->get_col(set_clause.lhs.col_name);
            if (lhs_col->type != TYPE_INT && lhs_col->type != TYPE_FLOAT) {
                return false;
            }
            Transaction::ReadColumnValue read_value{};
            return context_ != nullptr && context_->txn_ != nullptr &&
                   context_->txn_->get_read_column_value(tab_name_, rid, lhs_col->name, &read_value) &&
                   read_value.type == lhs_col->type && read_value.len == lhs_col->len;
        };
        auto can_rebase_after_committed_write = [&](const Rid &rid) {
            if (set_clauses_.empty()) {
                return false;
            }
            if (update_touches_index_columns_) {
                return false;
            }
            for (const auto &set_clause : set_clauses_) {
                if (!is_self_arithmetic_set_clause(set_clause) && !can_rebase_literal_assign(set_clause, rid)) {
                    return false;
                }
            }
            return true;
        };
        auto apply_read_delta_assign = [&](RmRecord *dst, const RmRecord *latest, const SetClause &set_clause,
                                           const Rid &rid) {
            if (!can_rebase_literal_assign(set_clause, rid)) {
                return;
            }
            auto lhs_col = tab_->get_col(set_clause.lhs.col_name);
            Transaction::ReadColumnValue read_value{};
            if (!context_->txn_->get_read_column_value(tab_name_, rid, lhs_col->name, &read_value)) {
                return;
            }
            const char *assigned = set_clause.rhs.raw->data;
            const char *read_old = read_value.data.data();
            const char *current = latest->data + lhs_col->offset;
            char *dest = dst->data + lhs_col->offset;
            if (lhs_col->type == TYPE_INT) {
                int delta = *reinterpret_cast<const int *>(assigned) - *reinterpret_cast<const int *>(read_old);
                int result = *reinterpret_cast<const int *>(current) + delta;
                memcpy(dest, &result, lhs_col->len);
            } else if (lhs_col->type == TYPE_FLOAT) {
                float delta = *reinterpret_cast<const float *>(assigned) - *reinterpret_cast<const float *>(read_old);
                float result = *reinterpret_cast<const float *>(current) + delta;
                memcpy(dest, &result, lhs_col->len);
            }
        };
        rmdb::bump_scan_cache_columns(tab_name_, changed_cols_);
        for (auto &rid : rids_) {
            if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr &&
                !context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd())) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            std::unique_ptr<RmRecord> rec;
            try {
                rec = fh_->get_record(rid, context_);
            } catch (RMDBError &) {
                throw;
            }
            bool use_latest_committed = false;
            TupleMeta current_meta{0, false};
            if (context_ != nullptr && context_->txn_mgr_ != nullptr) {
                current_meta = context_->txn_mgr_->GetTupleMetaOrDefault(tab_name_, rid);
                bool committed_after_snapshot =
                    current_meta.ts_ < TXN_START_ID && current_meta.ts_ > context_->txn_->get_read_ts();
                use_latest_committed = committed_after_snapshot && can_rebase_after_committed_write(rid);
                if (!use_latest_committed) {
                    context_->txn_mgr_->EnsureWriteConflictFree(context_->txn_, current_meta);
                    if (current_meta.is_deleted_) {
                        throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                        AbortReason::DEADLOCK_PREVENTION);
                    }
                } else if (current_meta.is_deleted_ || !eval_conds(tab_->cols, rec.get(), conds_)) {
                    continue;
                }
            }
            auto new_rec = std::make_unique<RmRecord>(*rec);
            for (auto &set_clause : compiled_set_clauses_) {
                set_clause.apply(new_rec->data, rec->data, set_clause);
            }
            if (use_latest_committed) {
                for (const auto &set_clause : set_clauses_) {
                    apply_read_delta_assign(new_rec.get(), rec.get(), set_clause, rid);
                }
            }
            if (memcmp(new_rec->data, rec->data, fh_->get_file_hdr().record_size) == 0) {
                continue;
            }
            if (key_conflict_check_required_ && context_ != nullptr && context_->txn_mgr_ != nullptr) {
                context_->txn_mgr_->EnsureKeyConflictFree(context_->txn_, tab_name_, *tab_, *new_rec, &rid);
            }
            effective_rids.push_back(rid);
            old_records.push_back(std::move(rec));
            new_records.push_back(std::move(new_rec));
            old_metas.push_back(current_meta);
        }

        if (update_touches_index_columns_) {
            for (size_t rec_idx = 0; rec_idx < new_records.size(); ++rec_idx) {
                for (size_t i = 0; i < index_bindings_.size(); ++i) {
                    if (!index_key_touched_[i]) {
                        continue;
                    }
                    const auto &binding = index_bindings_[i];
                    const auto &index = *binding.meta;
                    if (!index.unique) {
                        continue;
                    }
                    auto *ih = binding.ih;
                    char *key = rmdb::build_index_key_into(index, new_records[rec_idx]->data, &index_key_scratch_[i]);
                    std::vector<Rid> result;
                    if (ih->get_value(key, &result, context_->txn_)) {
                        for (auto &existing : result) {
                            if (existing != effective_rids[rec_idx]) {
                                auto conflict = context_->txn_mgr_ == nullptr
                                                    ? TransactionManager::UniqueKeyConflictResult::FAILURE
                                                    : context_->txn_mgr_->ClassifyUniqueIndexConflict(
                                                          context_ == nullptr ? nullptr : context_->txn_, tab_name_,
                                                          index, key, result, &effective_rids[rec_idx]);
                                if (conflict == TransactionManager::UniqueKeyConflictResult::ABORT) {
                                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                                    AbortReason::DEADLOCK_PREVENTION);
                                }
                                throw RMDBError("Duplicate key");
                            }
                        }
                    }
                }
            }
        }

        for (size_t rec_idx = 0; rec_idx < effective_rids.size(); ++rec_idx) {
            auto &old_rec = old_records[rec_idx];
            std::vector<InsertedIndexKey> inserted_index_keys;
            auto rollback_inserted_index_keys = [&]() {
                Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
                for (auto iter = inserted_index_keys.rbegin(); iter != inserted_index_keys.rend(); ++iter) {
                    iter->ih->delete_entry(iter->key.data(), txn);
                }
                inserted_index_keys.clear();
            };
            TransactionManager::record_serializable_write(context_ == nullptr ? nullptr : context_->txn_,
                                                          tab_name_, effective_rids[rec_idx], old_rec.get(),
                                                          new_records[rec_idx].get(), &tab_->cols);
            try {
                if (context_ != nullptr && context_->txn_ != nullptr) {
                    if (context_->txn_mgr_ != nullptr) {
                        TupleMeta old_meta = old_metas[rec_idx];
                        auto old_version = context_->txn_mgr_->GetVersionLink(tab_name_, effective_rids[rec_idx]);
                        auto undo_image = std::make_shared<RmRecord>(*old_rec);
                        UndoLog undo_log{old_meta.is_deleted_, {}, {}, undo_image, old_meta.ts_,
                                         old_version.has_value() ? old_version->prev_ : UndoLink{}};
                        UndoLink undo_link =
                            context_->txn_mgr_->AppendUndoLog(context_->txn_, std::move(undo_log));
                        context_->txn_mgr_->InstallTupleVersion(
                            tab_name_, effective_rids[rec_idx], VersionUndoLink{undo_link, true},
                            TupleMeta{TXN_START_ID + context_->txn_->get_transaction_id(), false});
                    }
                    context_->txn_->emplace_write_record(WType::UPDATE_TUPLE, tab_name_, effective_rids[rec_idx],
                                                         *old_rec, update_touches_index_columns_);
                }
                if (update_touches_index_columns_) {
                    for (size_t i = 0; i < index_bindings_.size(); ++i) {
                        if (!index_key_touched_[i]) {
                            continue;
                        }
                        const auto &binding = index_bindings_[i];
                        const auto &index = *binding.meta;
                        auto *ih = binding.ih;
                        char *old_key = rmdb::build_index_key_into(index, old_rec->data, effective_rids[rec_idx],
                                                                   &index_key_scratch_[i]);
                        char *new_key = rmdb::build_index_key_into(index, new_records[rec_idx]->data,
                                                                   effective_rids[rec_idx],
                                                                   &index_key_scratch_alt_[i]);
                        if (memcmp(old_key, new_key, index.col_tot_len) == 0) {
                            continue;
                        }
                        auto outcome = ih->insert_entry(new_key, effective_rids[rec_idx], context_->txn_);
                        if (outcome.result == IxInsertResult::kInserted) {
                            inserted_index_keys.push_back(
                                InsertedIndexKey{ih, std::string(new_key, index.col_tot_len)});
                        } else if (outcome.result == IxInsertResult::kDuplicate) {
                            if (!index.unique) {
                                continue;
                            }
                            std::vector<Rid> result;
                            ih->get_value(new_key, &result, context_->txn_);
                            auto conflict = context_->txn_mgr_ == nullptr
                                                ? TransactionManager::UniqueKeyConflictResult::FAILURE
                                                : context_->txn_mgr_->ClassifyUniqueIndexConflict(
                                                      context_ == nullptr ? nullptr : context_->txn_, tab_name_, index,
                                                      new_key, result, &effective_rids[rec_idx]);
                            if (conflict == TransactionManager::UniqueKeyConflictResult::ABORT) {
                                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                                AbortReason::DEADLOCK_PREVENTION);
                            }
                            throw RMDBError("Duplicate key");
                        }
                    }
                }
                PageId modified_page_id{};
                Page *modified_page = nullptr;
                bool defer_page_unpin = context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr;
                bool page_finalized = !defer_page_unpin;
                fh_->update_record(effective_rids[rec_idx], new_records[rec_idx]->data, context_,
                                   defer_page_unpin ? &modified_page_id : nullptr, defer_page_unpin,
                                   defer_page_unpin ? &modified_page : nullptr);
                if (context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
                    try {
                        UpdateLogRecord log_record(context_->txn_->get_transaction_id(), *old_rec,
                                                   *new_records[rec_idx], effective_rids[rec_idx], tab_name_);
                        log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                        lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                        context_->txn_->set_prev_lsn(lsn);
                        if (!sm_manager_->get_bpm()->finalize_page_write_fast(modified_page, modified_page_id, lsn,
                                                                              true)) {
                            sm_manager_->get_bpm()->finalize_page_write(modified_page_id, lsn, true);
                        }
                        page_finalized = true;
                    } catch (...) {
                        if (defer_page_unpin && !page_finalized) {
                            if (!sm_manager_->get_bpm()->unpin_page_fast(modified_page, modified_page_id, true)) {
                                sm_manager_->get_bpm()->unpin_page(modified_page_id, true);
                            }
                        }
                        throw;
                    }
                }
            } catch (...) {
                rollback_inserted_index_keys();
                throw;
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
