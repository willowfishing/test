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

enum AggType {
    AGG_COUNT,
    AGG_MAX,
    AGG_MIN,
    AGG_SUM,
    AGG_AVG
};

enum SetKnobType {
    EnableNestLoop, EnableSortMerge
};

enum IsolationChoice {
    IsolationSnapshot,
    IsolationSerializable
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

struct StaticCheckpoint : public TreeNode {
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
    std::string display;

    IntLit(int val_, std::string display_ = "")
        : val(val_), display(std::move(display_)) {}
};

struct FloatLit : public Value {
    float val;
    std::string display;

    FloatLit(float val_, std::string display_ = "")
        : val(val_), display(std::move(display_)) {}
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

struct SetClause : public TreeNode {
    std::string col_name;
    std::shared_ptr<Value> val;
    bool self_ref{false};
    std::string source_col;
    std::shared_ptr<Value> delta;

    SetClause(std::string col_name_, std::shared_ptr<Value> val_)
        : col_name(std::move(col_name_)), val(std::move(val_)) {}

    SetClause(std::string col_name_, std::string source_col_,
              std::shared_ptr<Value> delta_)
        : col_name(std::move(col_name_)), self_ref(true),
          source_col(std::move(source_col_)), delta(std::move(delta_)) {}
};

struct AggExpr : public Expr {
    AggType agg_type;
    bool star;
    std::shared_ptr<Col> col;

    AggExpr(AggType agg_type_, bool star_, std::shared_ptr<Col> col_ = nullptr)
        : agg_type(agg_type_), star(star_), col(std::move(col_)) {}
};

struct SelectItem : public TreeNode {
    std::shared_ptr<Expr> expr;
    std::string alias;

    SelectItem(std::shared_ptr<Expr> expr_, std::string alias_ = "")
        : expr(std::move(expr_)), alias(std::move(alias_)) {}
};

struct BinaryExpr : public TreeNode {
    std::shared_ptr<Expr> lhs;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    BinaryExpr(std::shared_ptr<Expr> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct OrderBy : public TreeNode {
    std::shared_ptr<Expr> expr;
    OrderByDir orderby_dir;

    OrderBy(std::shared_ptr<Expr> expr_, OrderByDir orderby_dir_)
        : expr(std::move(expr_)), orderby_dir(orderby_dir_) {}
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

struct TableRef {
    std::string tab_name;
    std::string alias;

    TableRef() = default;
    TableRef(std::string tab_name_, std::string alias_ = "")
        : tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}

    std::string qualifier() const { return alias.empty() ? tab_name : alias; }
};

struct FromClause {
    std::vector<TableRef> tables;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
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
    bool select_all{false};
    std::vector<std::shared_ptr<SelectItem>> select_items;
    // Plain columns are retained for compatibility with older diagnostics.
    std::vector<std::shared_ptr<Col>> cols;
    std::vector<TableRef> table_refs;
    // Keep the physical table names for compatibility with older code paths.
    std::vector<std::string> tabs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    std::vector<std::shared_ptr<JoinExpr>> jointree;
    std::vector<std::shared_ptr<Col>> group_by;
    std::vector<std::shared_ptr<BinaryExpr>> having;
    std::vector<std::shared_ptr<OrderBy>> orders;

    bool has_sort{false};
    bool explain_analyze{false};
    int limit{-1};

    SelectStmt(bool select_all_,
               std::vector<std::shared_ptr<SelectItem>> select_items_,
               std::vector<TableRef> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<Col>> group_by_ = {},
               std::vector<std::shared_ptr<BinaryExpr>> having_ = {},
               std::vector<std::shared_ptr<OrderBy>> orders_ = {},
               int limit_ = -1,
               bool explain_analyze_ = false)
        : select_all(select_all_), select_items(std::move(select_items_)),
          table_refs(std::move(table_refs_)), conds(std::move(conds_)),
          group_by(std::move(group_by_)), having(std::move(having_)),
          orders(std::move(orders_)), has_sort(!orders.empty()),
          explain_analyze(explain_analyze_), limit(limit_) {
        for (const auto &item : select_items) {
            if (auto col = std::dynamic_pointer_cast<Col>(item->expr)) {
                cols.push_back(std::move(col));
            }
        }
        for (const auto &table : table_refs) {
            tabs.push_back(table.tab_name);
        }
    }
};

struct UnionStmt : public TreeNode {
    std::vector<std::shared_ptr<SelectStmt>> branches;
    std::string alias;
    std::vector<std::shared_ptr<OrderBy>> orders;
    int limit{-1};
    bool explain_analyze{false};

    UnionStmt(std::vector<std::shared_ptr<SelectStmt>> branches_,
              std::string alias_,
              std::vector<std::shared_ptr<OrderBy>> orders_ = {},
              int limit_ = -1,
              bool explain_analyze_ = false)
        : branches(std::move(branches_)), alias(std::move(alias_)),
          orders(std::move(orders_)), limit(limit_),
          explain_analyze(explain_analyze_) {}
};

struct SetIsolation : public TreeNode {
    IsolationChoice level;
    explicit SetIsolation(IsolationChoice level_) : level(level_) {}
};

// set enable_nestloop
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;
    bool bool_val_;

    SetStmt(SetKnobType &type, bool bool_value) :
        set_knob_type_(type), bool_val_(bool_value) { }
};

// Semantic value
struct SemValue {
    int sv_int;
    float sv_float;
    std::string sv_str;
    bool sv_bool;
    OrderByDir sv_orderby_dir;
    std::vector<std::string> sv_strs;

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

    std::shared_ptr<SetClause> sv_set_clause;
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;

    std::shared_ptr<BinaryExpr> sv_cond;
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

    std::shared_ptr<OrderBy> sv_orderby;
    std::vector<std::shared_ptr<OrderBy>> sv_orderbys;
    std::shared_ptr<SelectItem> sv_select_item;
    std::vector<std::shared_ptr<SelectItem>> sv_select_items;
    AggType sv_agg_type;

    TableRef sv_table_ref;
    std::shared_ptr<FromClause> sv_from_clause;

    SetKnobType sv_setKnobType;
};

extern std::shared_ptr<ast::TreeNode> parse_tree;

}

#define YYSTYPE ast::SemValue
