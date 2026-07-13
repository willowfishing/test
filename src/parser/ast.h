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

#include <vector>
#include <string>
#include <memory>

#include "defs.h"
#include "transaction/txn_defs.h"

enum JoinType {
    INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN
};
namespace ast {

enum SvType {
    SV_TYPE_INT, SV_TYPE_FLOAT, SV_TYPE_STRING, SV_TYPE_BOOL
};

enum SvCompOp {
    SV_OP_EQ, SV_OP_NE, SV_OP_LT, SV_OP_GT, SV_OP_LE, SV_OP_GE
};

enum OrderByDir {
    OrderBy_DEFAULT,
    OrderBy_ASC,
    OrderBy_DESC
};

enum SetKnobType {
    EnableNestLoop, EnableSortMerge
};

// Base class for tree nodes
struct TreeNode {
    virtual ~TreeNode() = default;  // enable polymorphism
};

struct Help : public TreeNode {
};

struct ShowTables : public TreeNode {
};

struct ShowIndex : public TreeNode {
    std::string tab_name;

    ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct TxnBegin : public TreeNode {
};

struct TxnCommit : public TreeNode {
};

struct TxnAbort : public TreeNode {
};

struct TxnRollback : public TreeNode {
};

struct TypeLen : public TreeNode {
    SvType type;
    int len;

    TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

struct Field : public TreeNode {
};

struct ColDef : public Field {
    std::string col_name;
    std::shared_ptr<TypeLen> type_len;

    ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_) :
            col_name(std::move(col_name_)), type_len(std::move(type_len_)) {}
};

struct CreateTable : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Field>> fields;

    CreateTable(std::string tab_name_, std::vector<std::shared_ptr<Field>> fields_) :
            tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

struct DropTable : public TreeNode {
    std::string tab_name;

    DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct DescTable : public TreeNode {
    std::string tab_name;

    DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct CreateIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    CreateIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct DropIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    DropIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct Expr : public TreeNode {
};

struct Value : public Expr {
};

struct IntLit : public Value {
    int val;

    IntLit(int val_) : val(val_) {}
};

struct FloatLit : public Value {
    float val;
    std::string raw_text;

    FloatLit(float val_) : val(val_) {}
    FloatLit(float val_, std::string raw_text_) : val(val_), raw_text(std::move(raw_text_)) {}
};

struct StringLit : public Value {
    std::string val;

    StringLit(std::string val_) : val(std::move(val_)) {}
};

struct BoolLit : public Value {
    bool val;

    BoolLit(bool val_) : val(val_) {}
};

struct Col : public Expr {
    std::string tab_name;
    std::string col_name;

    Col(std::string tab_name_, std::string col_name_) :
            tab_name(std::move(tab_name_)), col_name(std::move(col_name_)) {}
};

struct AggFunc : public Expr {
    AggType agg_type;
    std::shared_ptr<Col> col;
    bool count_star;

    AggFunc(AggType agg_type_, std::shared_ptr<Col> col_, bool count_star_ = false)
        : agg_type(agg_type_), col(std::move(col_)), count_star(count_star_) {}
};

struct SelectItem : public TreeNode {
    std::shared_ptr<Expr> expr;
    std::string alias;

    SelectItem(std::shared_ptr<Expr> expr_, std::string alias_ = "")
        : expr(std::move(expr_)), alias(std::move(alias_)) {}
};

struct TableRef : public TreeNode {
    std::string tab_name;
    std::string alias;

    TableRef(std::string tab_name_, std::string alias_ = "")
        : tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}

    std::string display_name() const { return alias.empty() ? tab_name : alias; }
};

struct BinaryExpr;

struct FromClause : public TreeNode {
    std::vector<std::shared_ptr<TableRef>> table_refs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    bool has_explicit_join{false};

    FromClause() = default;
    FromClause(std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               bool has_explicit_join_ = false)
        : table_refs(std::move(table_refs_)), conds(std::move(conds_)), has_explicit_join(has_explicit_join_) {}
};

struct SetClause : public TreeNode {
    std::string col_name;
    std::shared_ptr<Value> val;
    std::string rhs_col_name;
    char rhs_op{0};
    std::shared_ptr<Value> rhs_delta;

    SetClause(std::string col_name_, std::shared_ptr<Value> val_) :
            col_name(std::move(col_name_)), val(std::move(val_)) {}
    SetClause(std::string col_name_, std::string rhs_col_name_, char rhs_op_, std::shared_ptr<Value> rhs_delta_) :
            col_name(std::move(col_name_)),
            rhs_col_name(std::move(rhs_col_name_)),
            rhs_op(rhs_op_),
            rhs_delta(std::move(rhs_delta_)) {}
};

struct BinaryExpr : public TreeNode {
    std::shared_ptr<Expr> lhs;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    BinaryExpr(std::shared_ptr<Expr> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct OrderBy : public TreeNode
{
    std::shared_ptr<Col> cols;
    OrderByDir orderby_dir;
    OrderBy( std::shared_ptr<Col> cols_, OrderByDir orderby_dir_) :
       cols(std::move(cols_)), orderby_dir(std::move(orderby_dir_)) {}
};

struct InsertStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Value>> vals;

    InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_) :
            tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

struct DeleteStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    DeleteStmt(std::string tab_name_, std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

struct UpdateStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<SetClause>> set_clauses;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    UpdateStmt(std::string tab_name_,
               std::vector<std::shared_ptr<SetClause>> set_clauses_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), set_clauses(std::move(set_clauses_)), conds(std::move(conds_)) {}
};

struct JoinExpr : public TreeNode {
    std::string left;
    std::string right;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    JoinType type;

    JoinExpr(std::string left_, std::string right_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_) :
            left(std::move(left_)), right(std::move(right_)), conds(std::move(conds_)), type(type_) {}
};

struct SelectStmt : public TreeNode {
    std::vector<std::shared_ptr<Col>> cols;
    std::vector<std::shared_ptr<SelectItem>> select_items;
    std::vector<std::string> tabs;
    std::vector<std::shared_ptr<TableRef>> table_refs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    std::vector<std::shared_ptr<Col>> group_by_cols;
    std::vector<std::shared_ptr<BinaryExpr>> having_conds;
    std::vector<std::shared_ptr<JoinExpr>> jointree;
    bool has_explicit_join{false};

    
    bool has_sort;
    std::vector<std::shared_ptr<OrderBy>> orders;
    std::shared_ptr<OrderBy> order;
    int limit{-1};


    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::string> tabs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::shared_ptr<OrderBy> order_) :
            cols(std::move(cols_)), tabs(std::move(tabs_)), conds(std::move(conds_)), 
            order(std::move(order_)) {
                has_sort = (bool)order;
                if (order) {
                    orders.push_back(order);
                }
            }

    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::shared_ptr<OrderBy> order_,
               bool has_explicit_join_ = false) :
            cols(std::move(cols_)), table_refs(std::move(table_refs_)), conds(std::move(conds_)),
            has_explicit_join(has_explicit_join_), order(std::move(order_)) {
                for (auto &ref : table_refs) {
                    tabs.push_back(ref->tab_name);
                }
                has_sort = (bool)order;
                if (order) {
                    orders.push_back(order);
                }
            }

    SelectStmt(std::vector<std::shared_ptr<SelectItem>> select_items_,
               std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<Col>> group_by_cols_,
               std::vector<std::shared_ptr<BinaryExpr>> having_conds_,
               std::vector<std::shared_ptr<OrderBy>> orders_,
               int limit_,
               bool has_explicit_join_ = false)
        : select_items(std::move(select_items_)),
          table_refs(std::move(table_refs_)),
          conds(std::move(conds_)),
          group_by_cols(std::move(group_by_cols_)),
          having_conds(std::move(having_conds_)),
          has_explicit_join(has_explicit_join_),
          orders(std::move(orders_)),
          limit(limit_) {
        for (auto &ref : table_refs) {
            tabs.push_back(ref->tab_name);
        }
        has_sort = !orders.empty();
        if (!orders.empty()) {
            order = orders.front();
        }
        for (auto &item : select_items) {
            if (auto col = std::dynamic_pointer_cast<Col>(item->expr)) {
                cols.push_back(col);
            }
        }
    }
};

struct ExplainAnalyze : public TreeNode {
    std::shared_ptr<SelectStmt> select;

    ExplainAnalyze(std::shared_ptr<SelectStmt> select_) : select(std::move(select_)) {}
};

// set enable_nestloop
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;
    bool bool_val_;

    SetStmt(SetKnobType &type, bool bool_value) : 
        set_knob_type_(type), bool_val_(bool_value) { }
};

struct SetIsolationStmt : public TreeNode {
    IsolationLevel isolation_level_;

    explicit SetIsolationStmt(IsolationLevel isolation_level) : isolation_level_(isolation_level) {}
};

// Semantic value
struct SemValue {
    int sv_int;
    float sv_float;
    std::string sv_str;
    bool sv_bool;
    OrderByDir sv_orderby_dir;
    std::vector<std::string> sv_strs;
    std::shared_ptr<TableRef> sv_table_ref;
    std::vector<std::shared_ptr<TableRef>> sv_table_refs;
    std::shared_ptr<FromClause> sv_from_clause;

    std::shared_ptr<TreeNode> sv_node;

    SvCompOp sv_comp_op;

    std::shared_ptr<TypeLen> sv_type_len;

    std::shared_ptr<Field> sv_field;
    std::vector<std::shared_ptr<Field>> sv_fields;

    std::shared_ptr<Expr> sv_expr;

    std::shared_ptr<Value> sv_val;
    std::vector<std::shared_ptr<Value>> sv_vals;

    std::shared_ptr<Col> sv_col;
    std::vector<std::shared_ptr<Col>> sv_cols;
    std::shared_ptr<SelectItem> sv_select_item;
    std::vector<std::shared_ptr<SelectItem>> sv_select_items;

    std::shared_ptr<SetClause> sv_set_clause;
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;

    std::shared_ptr<BinaryExpr> sv_cond;
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

    std::shared_ptr<OrderBy> sv_orderby;
    std::vector<std::shared_ptr<OrderBy>> sv_orderbys;

    SetKnobType sv_setKnobType;
};

extern std::shared_ptr<ast::TreeNode> parse_tree;

}

#define YYSTYPE ast::SemValue
