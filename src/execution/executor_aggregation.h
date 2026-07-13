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
#include "executor_seq_scan.h"
#include "index/ix.h"
#include "system/sm.h"
#include "optimizer/plan.h"

class AggregationExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<TabCol> group_by_;
    std::vector<AggInfo> aggs_;
    std::vector<Condition> having_;
    std::vector<TabCol> out_cols_;
    std::vector<ColMeta> cols_;          // output column metadata
    size_t len_;                         // output tuple length
    std::vector<std::unique_ptr<RmRecord>> results_;
    size_t current_idx_;

    // Helper: find column meta in input cols
    static const ColMeta *find_input_col(const std::vector<ColMeta> &input_cols, const TabCol &target) {
        for (const auto &c : input_cols) {
            if (c.tab_name == target.tab_name && c.name == target.col_name) {
                return &c;
            }
        }
        return nullptr;
    }

    // Helper: compute aggregate over a group of records
    // Returns a pair (float_val, is_int) — is_int is true only for COUNT and MAX/MIN on INT
    static float compute_agg(const AggInfo &agg,
                             const std::vector<std::unique_ptr<RmRecord>> &group_records,
                             const std::vector<ColMeta> &input_cols,
                             bool &is_int) {
        is_int = false;

        if (agg.agg_type == AggInfo::COUNT_ALL || agg.agg_type == AggInfo::COUNT_COL) {
            is_int = true;
            return static_cast<float>(group_records.size());
        }

        const ColMeta *col_meta = find_input_col(input_cols, agg.col);
        if (col_meta == nullptr || group_records.empty()) return 0.0f;

        bool input_is_int = (col_meta->type == TYPE_INT);

        if (agg.agg_type == AggInfo::MAX || agg.agg_type == AggInfo::MIN) {
            is_int = input_is_int;
            float best = 0.0f;
            bool first = true;
            for (auto &rec : group_records) {
                float val = 0.0f;
                if (input_is_int) {
                    val = static_cast<float>(*reinterpret_cast<const int *>(rec->data + col_meta->offset));
                } else {
                    val = *reinterpret_cast<const float *>(rec->data + col_meta->offset);
                }
                if (first) { best = val; first = false; continue; }
                if (agg.agg_type == AggInfo::MAX) {
                    if (val > best) best = val;
                } else {
                    if (val < best) best = val;
                }
            }
            return best;
        }

        // SUM or AVG
        // AVG always float; SUM inherits input type
        is_int = (agg.agg_type == AggInfo::SUM && input_is_int);
        float sum = 0.0f;
        int count = 0;
        for (auto &rec : group_records) {
            if (input_is_int) {
                sum += static_cast<float>(*reinterpret_cast<const int *>(rec->data + col_meta->offset));
            } else {
                sum += *reinterpret_cast<const float *>(rec->data + col_meta->offset);
            }
            count++;
        }
        if (agg.agg_type == AggInfo::AVG) {
            return count > 0 ? sum / count : 0.0f;
        }
        return sum;
    }

    // Check if a group satisfies HAVING conditions
    // group_records: raw input records for this group (for computing HAVING-only aggregates)
    // input_cols: column metadata of input records
    // 解析 "FUNC(col)" 格式列名 (大小写不敏感)
    static std::string agg_lower(const std::string &s) {
        std::string r = s;
        for (auto &c : r) c = tolower(c);
        return r;
    }
    static bool parse_agg_name(const std::string &cname, int &agg_type, std::string &inner, bool &is_star) {
        is_star = false;
        auto paren = cname.find('(');
        if (paren == std::string::npos) return false;
        std::string func = agg_lower(cname.substr(0, paren));
        inner = cname.substr(paren + 1);
        if (!inner.empty() && inner.back() == ')') inner.pop_back();
        if (func == "count") {
            if (inner == "*") { is_star = true; agg_type = AggInfo::COUNT_ALL; }
            else agg_type = AggInfo::COUNT_COL;
        } else if (func == "max") agg_type = AggInfo::MAX;
        else if (func == "min") agg_type = AggInfo::MIN;
        else if (func == "sum") agg_type = AggInfo::SUM;
        else if (func == "avg") agg_type = AggInfo::AVG;
        else return false;
        return true;
    }

    bool check_having(const std::vector<Condition> &having,
                      const std::vector<ColMeta> &group_cols,
                      const RmRecord &group_output_rec,
                      const std::vector<std::unique_ptr<RmRecord>> &group_records,
                      const std::vector<ColMeta> &input_cols) {
        for (auto &cond : having) {
            if (!cond.is_rhs_val) continue;
            std::string cname = cond.lhs_col.col_name;

            // 1. 精确匹配输出列
            const ColMeta *col = find_input_col(group_cols, cond.lhs_col);

            // 2. 通过聚合别名或函数名匹配
            if (col == nullptr) {
                int c_at; std::string c_inner; bool c_star;
                bool c_is_agg = parse_agg_name(cname, c_at, c_inner, c_star);
                for (auto &agg : aggs_) {
                    if (!agg.alias.empty() && agg_lower(cname) == agg_lower(agg.alias)) {
                        TabCol out_col = {.tab_name = agg.col.tab_name, .col_name = agg.alias};
                        col = find_input_col(group_cols, out_col);
                        if (col) break;
                    }
                    if (c_is_agg && (int)agg.agg_type == c_at) {
                        if (c_star && agg.agg_type == AggInfo::COUNT_ALL) {
                            std::string out_name = agg.alias.empty() ? "COUNT(*)" : agg.alias;
                            TabCol out_col = {.tab_name = "", .col_name = out_name};
                            col = find_input_col(group_cols, out_col);
                            if (col) break;
                        } else if (!c_star && agg_lower(agg.col.col_name) == agg_lower(c_inner)) {
                            std::string out_name = agg.alias.empty() ? cname : agg.alias;
                            TabCol out_col = {.tab_name = agg.col.tab_name, .col_name = out_name};
                            col = find_input_col(group_cols, out_col);
                            if (col) break;
                        }
                    }
                }
            }

            // 3. HAVING 独有的聚合：直接计算
            if (col == nullptr) {
                int at; std::string inner; bool is_star;
                if (parse_agg_name(cname, at, inner, is_star)) {
                                        AggInfo hav_agg;
                    hav_agg.agg_type = static_cast<AggInfo::AggType>(at);
                    if (!is_star && !inner.empty()) {
                        for (auto &ic : input_cols) {
                            if (ic.name == inner) { hav_agg.col = {ic.tab_name, ic.name}; break; }
                        }
                    }
                    bool is_int = false;
                    float val = compute_agg(hav_agg, group_records, input_cols, is_int);
                                        int cmp_len = is_int ? sizeof(int) : sizeof(float);
                    if (cond.rhs_val.raw == nullptr)
                        const_cast<Value &>(cond.rhs_val).init_raw(cmp_len);
                    char agg_buf[8];
                    if (is_int) *(int*)agg_buf = static_cast<int>(val);
                    else *(float*)agg_buf = val;
                    ColType agg_ct = is_int ? TYPE_INT : TYPE_FLOAT;
                    int cmp = SeqScanExecutor::compare_value(agg_ct, cond.rhs_val.type,
                                                              agg_buf, cond.rhs_val.raw->data,
                                                              cmp_len, cmp_len);
                                        if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
                    continue;
                }
                continue;
            }

            // 4. 从输出列比较
            if (cond.rhs_val.raw == nullptr)
                const_cast<Value &>(cond.rhs_val).init_raw(col->len);
            const char *data = group_output_rec.data + col->offset;
            int cmp = SeqScanExecutor::compare_value(col->type, cond.rhs_val.type, data,
                                                      cond.rhs_val.raw->data, col->len, col->len);
                        if (!SeqScanExecutor::check_cmp(cmp, cond.op)) return false;
        }
        return true;
    }

   public:
    AggregationExecutor(std::unique_ptr<AbstractExecutor> prev,
                        std::vector<TabCol> group_by,
                        std::vector<AggInfo> aggs,
                        std::vector<Condition> having,
                        std::vector<TabCol> out_cols) {
        prev_ = std::move(prev);
        group_by_ = std::move(group_by);
        aggs_ = std::move(aggs);
        having_ = std::move(having);
        out_cols_ = std::move(out_cols);
        current_idx_ = 0;

        // Build output column metadata
        size_t curr_offset = 0;
        auto &input_cols = prev_->cols();
        std::vector<TabCol> output_order;
        if (!out_cols_.empty()) {
            output_order = out_cols_;
        } else {
            for (auto &gb : group_by_) output_order.push_back(gb);
            for (auto &agg : aggs_) {
                TabCol ac;
                if (agg.agg_type == AggInfo::COUNT_ALL) {
                    ac.tab_name = "";
                    ac.col_name = agg.alias.empty() ? "COUNT(*)" : agg.alias;
                } else {
                    ac.tab_name = agg.col.tab_name;
                    ac.col_name = agg.alias.empty() ? agg.col.col_name : agg.alias;
                }
                output_order.push_back(ac);
            }
        }

        for (auto &out_col : output_order) {
            // Check if this is from group_by
            bool is_gb = false;
            for (auto &gb : group_by_) {
                if (gb.tab_name == out_col.tab_name && gb.col_name == out_col.col_name) {
                    const ColMeta *ic = find_input_col(input_cols, gb);
                    if (ic) {
                        ColMeta col = *ic;
                        col.offset = curr_offset;
                        curr_offset += col.len;
                        cols_.push_back(col);
                        is_gb = true;
                    }
                    break;
                }
            }
            if (is_gb) continue;

            // Check if this is an aggregate column
            for (auto &agg : aggs_) {
                // 生成与此聚合对应的唯一列名
                std::string agg_name;
                if (agg.agg_type == AggInfo::COUNT_ALL) {
                    agg_name = agg.alias.empty() ? "COUNT(*)" : agg.alias;
                } else if (agg.alias.empty()) {
                    const char* agg_names[] = {"COUNT_ALL", "COUNT", "MAX", "MIN", "SUM", "AVG"};
                    agg_name = std::string(agg_names[agg.agg_type]) + "(" + agg.col.col_name + ")";
                } else {
                    agg_name = agg.alias;
                }
                if (out_col.col_name == agg_name) {
                    ColMeta ac;
                    ac.tab_name = agg.col.tab_name;
                    ac.name = out_col.col_name;

                    if (agg.agg_type == AggInfo::COUNT_ALL || agg.agg_type == AggInfo::COUNT_COL) {
                        ac.type = TYPE_INT;
                        ac.len = sizeof(int);
                    } else if (agg.agg_type == AggInfo::AVG) {
                        ac.type = TYPE_FLOAT;
                        ac.len = sizeof(float);
                    } else {
                        // MAX/MIN/SUM: 继承输入列类型
                        const ColMeta *ic = find_input_col(input_cols, agg.col);
                        ac.type = (ic && ic->type == TYPE_INT) ? TYPE_INT : TYPE_FLOAT;
                        ac.len = (ac.type == TYPE_INT) ? sizeof(int) : sizeof(float);
                    }
                    ac.offset = curr_offset;
                    curr_offset += ac.len;
                    cols_.push_back(ac);
                    break;
                }
            }
        }
        len_ = curr_offset;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    void beginTuple() override {
        results_.clear();
        auto &input_cols = prev_->cols();

        if (group_by_.empty()) {
            // No GROUP BY: compute aggregates over ALL rows
            std::vector<std::unique_ptr<RmRecord>> all_records;
            for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
                all_records.push_back(prev_->Next());
            }

            auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
            for (size_t i = 0; i < cols_.size(); i++) {
                auto &col = cols_[i];
                for (auto &agg : aggs_) {
                    std::string agg_name;
                    if (agg.agg_type == AggInfo::COUNT_ALL) {
                        agg_name = agg.alias.empty() ? "COUNT(*)" : agg.alias;
                    } else if (agg.alias.empty()) {
                        const char* agg_names[] = {"COUNT_ALL", "COUNT", "MAX", "MIN", "SUM", "AVG"};
                        agg_name = std::string(agg_names[agg.agg_type]) + "(" + agg.col.col_name + ")";
                    } else {
                        agg_name = agg.alias;
                    }
                    if (col.name == agg_name) {
                        bool is_int = false;
                        float val = compute_agg(agg, all_records, input_cols, is_int);
                        if (is_int) {
                            *reinterpret_cast<int *>(rec->data + col.offset) = static_cast<int>(val);
                        } else {
                            *reinterpret_cast<float *>(rec->data + col.offset) = val;
                        }
                        break;
                    }
                }
            }
            if (check_having(having_, cols_, *rec, all_records, input_cols)) {
                results_.push_back(std::move(rec));
            }
        } else {
            // With GROUP BY
            auto &in_cols = prev_->cols();
            std::map<std::string, std::vector<std::unique_ptr<RmRecord>>> groups;
            std::vector<std::string> group_order;

            int key_len = 0;
            for (auto &gb : group_by_) {
                const ColMeta *ic = find_input_col(in_cols, gb);
                if (ic) key_len += ic->len;
            }

            for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
                auto rec = prev_->Next();
                std::string key;
                key.reserve(key_len);
                for (auto &gb : group_by_) {
                    const ColMeta *ic = find_input_col(in_cols, gb);
                    if (ic) key.append(rec->data + ic->offset, ic->len);
                }
                if (groups.find(key) == groups.end()) {
                    group_order.push_back(key);
                }
                auto rec_copy = std::make_unique<RmRecord>(static_cast<int>(prev_->tupleLen()));
                memcpy(rec_copy->data, rec->data, prev_->tupleLen());
                groups[key].push_back(std::move(rec_copy));
            }

            for (auto &key : group_order) {
                auto &group_recs = groups[key];
                auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));

                for (size_t i = 0; i < cols_.size(); i++) {
                    auto &col = cols_[i];
                    // Copy GROUP BY value from first record
                    bool is_gb = false;
                    for (auto &gb : group_by_) {
                        const ColMeta *ic = find_input_col(in_cols, gb);
                        if (ic && ic->tab_name == col.tab_name && ic->name == col.name) {
                            memcpy(rec->data + col.offset,
                                   group_recs[0]->data + ic->offset, col.len);
                            is_gb = true;
                            break;
                        }
                    }
                    if (is_gb) continue;

                    // Compute aggregate
                    for (auto &agg : aggs_) {
                        std::string agg_name = agg.alias.empty() ? agg.col.col_name : agg.alias;
                        if (agg.agg_type == AggInfo::COUNT_ALL) {
                            agg_name = agg.alias.empty() ? "COUNT(*)" : agg.alias;
                        }
                        if (col.name == agg_name) {
                            bool is_int = false;
                            float val = compute_agg(agg, group_recs, input_cols, is_int);
                            if (is_int) {
                                *reinterpret_cast<int *>(rec->data + col.offset) = static_cast<int>(val);
                            } else {
                                *reinterpret_cast<float *>(rec->data + col.offset) = val;
                            }
                            break;
                        }
                    }
                }
                if (check_having(having_, cols_, *rec, group_recs, input_cols)) {
                    results_.push_back(std::move(rec));
                }
            }
        }

        current_idx_ = 0;
    }

    void nextTuple() override {
        current_idx_++;
    }

    bool is_end() const override {
        return current_idx_ >= results_.size();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_idx_ < results_.size()) {
            auto rec = std::make_unique<RmRecord>(static_cast<int>(len_));
            memcpy(rec->data, results_[current_idx_]->data, len_);
            return rec;
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
