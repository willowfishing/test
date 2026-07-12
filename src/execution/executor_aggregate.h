#pragma once

#include <map>
#include <set>
#include <sstream>

#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    struct Cell {
        ColType type = TYPE_INT;
        int int_val = 0;
        float float_val = 0;
        std::string str_val;
        int count = 0;
        bool initialized = false;
    };

    struct GroupState {
        std::vector<Cell> group_cells;
        std::map<std::string, Cell> aggs;
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<std::shared_ptr<ast::SelectItem>> select_items_;
    std::vector<TabCol> group_cols_;
    std::vector<std::shared_ptr<ast::HavingExpr>> having_conds_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_;

    std::string agg_key(const std::shared_ptr<ast::SelectItem> &item) const {
        std::string key = std::to_string(static_cast<int>(item->agg_type)) + ":";
        if (item->count_star) {
            return key + "*";
        }
        return key + item->col->tab_name + "." + item->col->col_name;
    }

    Cell read_cell(const RmRecord *rec, const ColMeta &col) const {
        Cell cell;
        cell.type = col.type;
        cell.initialized = true;
        if (col.type == TYPE_INT) {
            cell.int_val = *reinterpret_cast<int *>(rec->data + col.offset);
        } else if (col.type == TYPE_FLOAT) {
            cell.float_val = *reinterpret_cast<float *>(rec->data + col.offset);
        } else {
            cell.str_val = std::string(rec->data + col.offset, col.len);
            cell.str_val.resize(strlen(cell.str_val.c_str()));
        }
        return cell;
    }

    int compare_cell(const Cell &lhs, const Cell &rhs) const {
        if (lhs.type == TYPE_FLOAT || rhs.type == TYPE_FLOAT) {
            float l = lhs.type == TYPE_FLOAT ? lhs.float_val : static_cast<float>(lhs.int_val);
            float r = rhs.type == TYPE_FLOAT ? rhs.float_val : static_cast<float>(rhs.int_val);
            return (l > r) - (l < r);
        }
        if (lhs.type == TYPE_INT) {
            return (lhs.int_val > rhs.int_val) - (lhs.int_val < rhs.int_val);
        }
        return lhs.str_val.compare(rhs.str_val);
    }

    std::string group_key(const std::vector<Cell> &cells) const {
        std::ostringstream os;
        for (auto &cell : cells) {
            if (cell.type == TYPE_INT) {
                os << "i" << cell.int_val << "|";
            } else if (cell.type == TYPE_FLOAT) {
                os << "f" << cell.float_val << "|";
            } else {
                os << "s" << cell.str_val << "|";
            }
        }
        return os.str();
    }

    Cell rhs_cell(const std::shared_ptr<ast::Value> &value) const {
        Cell cell;
        cell.initialized = true;
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
            cell.type = TYPE_INT;
            cell.int_val = int_lit->val;
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
            cell.type = TYPE_FLOAT;
            cell.float_val = float_lit->val;
        } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
            cell.type = TYPE_STRING;
            cell.str_val = str_lit->val;
        }
        return cell;
    }

    bool compare_having(int cmp, ast::SvCompOp op) const {
        switch (op) {
            case ast::SV_OP_EQ: return cmp == 0;
            case ast::SV_OP_NE: return cmp != 0;
            case ast::SV_OP_LT: return cmp < 0;
            case ast::SV_OP_GT: return cmp > 0;
            case ast::SV_OP_LE: return cmp <= 0;
            case ast::SV_OP_GE: return cmp >= 0;
        }
        return false;
    }

    bool pass_having(const GroupState &state) const {
        for (auto &cond : having_conds_) {
            auto lhs = state.aggs.at(agg_key(cond->lhs));
            if (cond->lhs->agg_type == ast::AGG_AVG && lhs.count > 0) {
                lhs.float_val /= static_cast<float>(lhs.count);
            }
            auto rhs = rhs_cell(cond->rhs);
            if (!compare_having(compare_cell(lhs, rhs), cond->op)) {
                return false;
            }
        }
        return true;
    }

    void write_cell(RmRecord *rec, const ColMeta &col, const Cell &cell) const {
        if (col.type == TYPE_INT) {
            *reinterpret_cast<int *>(rec->data + col.offset) = cell.int_val;
        } else if (col.type == TYPE_FLOAT) {
            float value = cell.type == TYPE_FLOAT ? cell.float_val : static_cast<float>(cell.int_val);
            *reinterpret_cast<float *>(rec->data + col.offset) = value;
        } else {
            memset(rec->data + col.offset, 0, col.len);
            memcpy(rec->data + col.offset, cell.str_val.c_str(), std::min<int>(col.len, cell.str_val.size()));
        }
    }

    void update_agg(Cell &agg, const Cell &input, ast::AggType type) {
        if (type == ast::AGG_COUNT) {
            agg.type = TYPE_INT;
            agg.int_val++;
            agg.initialized = true;
            return;
        }
        if (type == ast::AGG_SUM) {
            agg.type = input.type == TYPE_FLOAT ? TYPE_FLOAT : TYPE_INT;
            if (input.type == TYPE_FLOAT) {
                agg.float_val += input.float_val;
            } else {
                agg.int_val += input.int_val;
            }
            agg.initialized = true;
            return;
        }
        if (type == ast::AGG_AVG) {
            agg.type = TYPE_FLOAT;
            agg.float_val += input.type == TYPE_FLOAT ? input.float_val : static_cast<float>(input.int_val);
            agg.count++;
            agg.initialized = true;
            return;
        }
        if (!agg.initialized || (type == ast::AGG_MAX && compare_cell(input, agg) > 0) ||
            (type == ast::AGG_MIN && compare_cell(input, agg) < 0)) {
            agg = input;
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev,
                      std::vector<std::shared_ptr<ast::SelectItem>> select_items,
                      std::vector<TabCol> group_cols,
                      std::vector<std::shared_ptr<ast::HavingExpr>> having_conds,
                      std::vector<TabCol> output_cols) {
        prev_ = std::move(prev);
        select_items_ = std::move(select_items);
        group_cols_ = std::move(group_cols);
        having_conds_ = std::move(having_conds);
        len_ = 0;
        for (size_t i = 0; i < select_items_.size(); ++i) {
            ColMeta col;
            col.tab_name = "";
            col.name = output_cols[i].col_name;
            col.offset = len_;
            auto &item = select_items_[i];
            if (!item->is_agg) {
                auto src = prev_->get_col_offset({item->col->tab_name, item->col->col_name});
                col.type = src.type;
                col.len = src.len;
            } else if (item->agg_type == ast::AGG_COUNT) {
                col.type = TYPE_INT;
                col.len = sizeof(int);
            } else if (item->agg_type == ast::AGG_AVG) {
                col.type = TYPE_FLOAT;
                col.len = sizeof(float);
            } else {
                auto src = prev_->get_col_offset({item->col->tab_name, item->col->col_name});
                col.type = src.type;
                col.len = src.len;
            }
            len_ += col.len;
            cols_.push_back(col);
        }
        cursor_ = 0;
    }

    void beginTuple() override {
        tuples_.clear();
        std::map<std::string, GroupState> groups;
        std::vector<std::string> order;
        std::vector<std::shared_ptr<ast::SelectItem>> agg_items;
        std::set<std::string> agg_keys;
        for (auto &item : select_items_) {
            if (item->is_agg && agg_keys.insert(agg_key(item)).second) {
                agg_items.push_back(item);
            }
        }
        for (auto &cond : having_conds_) {
            if (agg_keys.insert(agg_key(cond->lhs)).second) {
                agg_items.push_back(cond->lhs);
            }
        }
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            std::vector<Cell> group_cells;
            for (auto &group_col : group_cols_) {
                group_cells.push_back(read_cell(rec.get(), prev_->get_col_offset(group_col)));
            }
            std::string key = group_cols_.empty() ? "__all__" : group_key(group_cells);
            if (groups.find(key) == groups.end()) {
                groups[key].group_cells = group_cells;
                order.push_back(key);
            }
            auto &state = groups[key];
            for (auto &item : agg_items) {
                Cell input;
                if (!item->count_star) {
                    input = read_cell(rec.get(), prev_->get_col_offset({item->col->tab_name, item->col->col_name}));
                }
                update_agg(state.aggs[agg_key(item)], input, item->agg_type);
            }
        }

        if (groups.empty() && group_cols_.empty()) {
            groups["__all__"] = GroupState();
            order.push_back("__all__");
            for (auto &item : select_items_) {
                if (item->is_agg && item->agg_type == ast::AGG_COUNT) {
                    Cell zero;
                    zero.type = TYPE_INT;
                    zero.int_val = 0;
                    zero.initialized = true;
                    groups["__all__"].aggs[agg_key(item)] = zero;
                }
            }
            for (auto &cond : having_conds_) {
                if (cond->lhs->agg_type == ast::AGG_COUNT) {
                    Cell zero;
                    zero.type = TYPE_INT;
                    zero.int_val = 0;
                    zero.initialized = true;
                    groups["__all__"].aggs[agg_key(cond->lhs)] = zero;
                }
            }
        }

        for (auto &key : order) {
            auto &state = groups[key];
            if (!pass_having(state)) {
                continue;
            }
            auto out = std::make_unique<RmRecord>(len_);
            for (size_t i = 0; i < select_items_.size(); ++i) {
                auto &item = select_items_[i];
                Cell cell;
                if (item->is_agg) {
                    cell = state.aggs.at(agg_key(item));
                    if (item->agg_type == ast::AGG_AVG && cell.count > 0) {
                        cell.float_val /= static_cast<float>(cell.count);
                    }
                } else {
                    for (size_t group_idx = 0; group_idx < group_cols_.size(); ++group_idx) {
                        if (group_cols_[group_idx].tab_name == item->col->tab_name &&
                            group_cols_[group_idx].col_name == item->col->col_name) {
                            cell = state.group_cells[group_idx];
                            break;
                        }
                    }
                }
                write_cell(out.get(), cols_[i], cell);
            }
            tuples_.push_back(std::move(out));
        }
        cursor_ = 0;
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "AggregateExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, tuples_[cursor_]->data, len_);
        return rec;
    }
};
