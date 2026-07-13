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
#include "transaction/mvcc_manager.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator
    std::vector<MvccRecord> visible_records_;
    size_t visible_pos_{0};

    SmManager *sm_manager_;

    void advance_to_next_match() {
        if (!visible_records_.empty() || scan_ == nullptr) {
            while (visible_pos_ < visible_records_.size()) {
                rid_ = visible_records_[visible_pos_].rid;
                auto &rec = visible_records_[visible_pos_].record;
                if (eval_conds(cols_, rec.get(), fed_conds_)) {
                    MvccManager::RegisterRecordRead(context_->txn_, tab_name_, rid_);
                    return;
                }
                visible_pos_++;
            }
            return;
        }
        while (scan_ != nullptr && !scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, rec.get(), fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context,
                    std::string display_tab_name = "") {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        std::string visible_tab_name = display_tab_name.empty() ? tab_name_ : std::move(display_tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        for (auto &col : cols_) {
            col.tab_name = visible_tab_name;
        }
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "SeqScanExecutor"; }

    bool is_end() const override {
        if (!visible_records_.empty() || scan_ == nullptr) {
            return visible_pos_ >= visible_records_.size();
        }
        return scan_->is_end();
    }

    void beginTuple() override {
        MvccManager::RegisterPredicateRead(context_->txn_, tab_name_, cols_, fed_conds_);
        visible_records_ = MvccManager::CollectVisibleRecords(sm_manager_, tab_name_, context_->txn_);
        visible_pos_ = 0;
        scan_.reset();
        advance_to_next_match();
    }

    void nextTuple() override {
        if (!visible_records_.empty() || scan_ == nullptr) {
            if (visible_pos_ < visible_records_.size()) {
                visible_pos_++;
            }
            advance_to_next_match();
            return;
        }
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        if (!visible_records_.empty() || scan_ == nullptr) {
            return std::make_unique<RmRecord>(*visible_records_[visible_pos_].record);
        }
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }
};
