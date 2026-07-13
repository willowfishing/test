%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;

// 全局变量：收集 JOIN ON 条件
static std::vector<std::shared_ptr<BinaryExpr>> g_on_conds;
// 全局变量：收集 UNION 子查询 (共享 ast.cpp 中的 g_union_map)
// 通过 ast::get_union_map() 访问
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY
WHERE UPDATE SET SELECT INT CHAR FLOAT INDEX AND JOIN ON EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE EXPLAIN ANALYZE
%token COUNT MAX MIN SUM AVG GROUP HAVING LIMIT AS UNION
%token TRANSACTION ISOLATION SNAPSHOT LEVEL SERIALIZABLE
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> colNameList
%type <sv_col> col
%type <sv_cols> colList
%type <sv_cols> selector
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_orderby>  order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_setKnobType> set_knob_type
%type <sv_int> opt_limit
%type <sv_cols> opt_group_by
%type <sv_conds> opt_having

// tableRef returns pair<table_name, alias> as two strings via sv_strs
%type <sv_strs> tableRefs tableRef

// UNION support
%type <sv_node> selectWithin unionQuery

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    | SHOW INDEX FROM tbName
    {
        auto *n = new ShowIndex();
        n->tab_name = $4;
        $$ = std::shared_ptr<ShowIndex>(n);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION
    {
        $$ = std::make_shared<SetIsolationLevel>(IsoLevelSyntax::SNAPSHOT_ISOLATION);
    }
    |   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
    {
        $$ = std::make_shared<SetIsolationLevel>(IsoLevelSyntax::SERIALIZABLE);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having opt_order_clause opt_limit
    {
        auto conds = $5;
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < $4.size(); i += 2) {
            tables.push_back($4[i]);
            if (i + 1 < $4.size()) {
                aliases.push_back($4[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto stmt = std::make_shared<SelectStmt>($2, tables, aliases, std::move(conds), $8, $6, $7, $9);
        $$ = stmt;
    }
    |   EXPLAIN ANALYZE SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having opt_order_clause opt_limit
    {
        auto conds = $7;
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < $6.size(); i += 2) {
            tables.push_back($6[i]);
            if (i + 1 < $6.size()) {
                aliases.push_back($6[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto select = std::make_shared<SelectStmt>($4, tables, aliases, std::move(conds), $10, $8, $9, $11);
        select->is_explain_analyze = true;
        $$ = select;
    }
    ;

// selectWithin: SELECT body without ORDER BY/LIMIT (only used inside unionQuery)
selectWithin:
        SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having
    {
        auto conds = $5;
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < $4.size(); i += 2) {
            tables.push_back($4[i]);
            if (i + 1 < $4.size()) {
                aliases.push_back($4[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto stmt = std::make_shared<SelectStmt>($2, tables, aliases, std::move(conds), nullptr, $6, $7, -1);
        $$ = std::static_pointer_cast<TreeNode>(stmt);
    }
    ;

unionQuery:
        selectWithin UNION selectWithin
    {
        auto u = std::make_shared<UnionStmt>();
        u->selects.push_back(std::static_pointer_cast<SelectStmt>($1));
        u->selects.push_back(std::static_pointer_cast<SelectStmt>($3));
        $$ = std::static_pointer_cast<TreeNode>(u);
    }
    |   unionQuery UNION selectWithin
    {
        auto u = std::static_pointer_cast<UnionStmt>($1);
        u->selects.push_back(std::static_pointer_cast<SelectStmt>($3));
        $$ = $1;
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<BinaryExpr>>(); }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   tbName '.' colName AS IDENTIFIER
    {
        auto c = std::make_shared<Col>($1, $3); c->alias = $5; $$ = c;
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    |   colName AS IDENTIFIER
    {
        auto c = std::make_shared<Col>("", $1); c->alias = $3; $$ = c;
    }
    |   COUNT '(' '*' ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->is_star = true;
        auto c = std::make_shared<Col>("", "COUNT(*)"); c->agg = a; $$ = c;
    }
    |   COUNT '(' '*' ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->is_star = true;
        auto c = std::make_shared<Col>("", "COUNT(*)"); c->agg = a; c->alias = $6; $$ = c;
    }
    |   COUNT '(' col ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "COUNT(" + $3->col_name + ")"); c->agg = a; $$ = c;
    }
    |   COUNT '(' col ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "COUNT(" + $3->col_name + ")"); c->agg = a; c->alias = $6; $$ = c;
    }
    |   MAX '(' col ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 1; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "MAX(" + $3->col_name + ")"); c->agg = a; $$ = c;
    }
    |   MAX '(' col ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 1; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "MAX(" + $3->col_name + ")"); c->agg = a; c->alias = $6; $$ = c;
    }
    |   MIN '(' col ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 2; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "MIN(" + $3->col_name + ")"); c->agg = a; $$ = c;
    }
    |   MIN '(' col ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 2; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "MIN(" + $3->col_name + ")"); c->agg = a; c->alias = $6; $$ = c;
    }
    |   SUM '(' col ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 3; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "SUM(" + $3->col_name + ")"); c->agg = a; $$ = c;
    }
    |   SUM '(' col ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 3; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "SUM(" + $3->col_name + ")"); c->agg = a; c->alias = $6; $$ = c;
    }
    |   AVG '(' col ')'
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 4; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "AVG(" + $3->col_name + ")"); c->agg = a; $$ = c;
    }
    |   AVG '(' col ')' AS IDENTIFIER
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 4; a->col = $3;
        auto c = std::make_shared<Col>($3->tab_name, "AVG(" + $3->col_name + ")"); c->agg = a; c->alias = $6; $$ = c;
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    |   colName '=' colName '+' value
    {
        auto sc = std::make_shared<SetClause>($1, $5);
        sc->is_self_ref = true;
        sc->self_ref_col = $3;
        $$ = sc;
    }
    |   colName '=' colName '-' value
    {
        // Negate the value and treat as addition
        if (auto il = std::dynamic_pointer_cast<ast::IntLit>($5)) {
            il->val = -il->val;
        } else if (auto fl = std::dynamic_pointer_cast<ast::FloatLit>($5)) {
            fl->val = -fl->val;
        }
        auto sc = std::make_shared<SetClause>($1, $5);
        sc->is_self_ref = true;
        sc->self_ref_col = $3;
        $$ = sc;
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   colList
    {
        $$ = $1;
    }
    ;

opt_group_by:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<Col>>(); }
    |   GROUP BY colList
    {
        $$ = $3;
    }
    ;

opt_having:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<BinaryExpr>>(); }
    |   HAVING whereClause
    {
        $$ = $2;
    }
    ;

opt_limit:
        /* epsilon */ { $$ = -1; }
    |   LIMIT VALUE_INT
    {
        $$ = $2;
    }
    ;

// 支持表别名的表引用列表
tableRefs:
        tableRef
    {
        $$ = $1;
    }
    |   tableRefs ',' tableRef
    {
        $$.insert($$.end(), $3.begin(), $3.end());
    }
    |   tableRefs JOIN tableRef ON whereClause
    {
        // 把 ON 条件保存到全局变量中
        for (auto &cond : $5) {
            g_on_conds.push_back(cond);
        }
        $$.insert($$.end(), $3.begin(), $3.end());
    }
    |   tableRefs JOIN tableRef
    {
        $$.insert($$.end(), $3.begin(), $3.end());
    }
    ;

// tableRef 始终返回 {table_name, alias}，无别名时 alias 为空
// 对于 UNION 派生表，table_name 为别名，alias 为空，UnionStmt 存入 g_union_map
tableRef:
        tbName
    {
        $$ = std::vector<std::string>{$1, ""};
    }
    |   tbName tbName
    {
        // 表名 + 别名：table_name alias_name
        $$ = std::vector<std::string>{$1, $2};
    }
    |   tbName AS tbName
    {
        // 表名 + AS 别名
        $$ = std::vector<std::string>{$1, $3};
    }
    |   '(' unionQuery ')' tbName
    {
        ast::get_union_map()[$4] = std::static_pointer_cast<UnionStmt>($2);
        $$ = std::vector<std::string>{$4, $4};
    }
    |   '(' unionQuery ')' AS tbName
    {
        ast::get_union_map()[$5] = std::static_pointer_cast<UnionStmt>($2);
        $$ = std::vector<std::string>{$5, $5};
    }
    ;

opt_order_clause:
    ORDER BY order_clause
    {
        $$ = $3;
    }
    |   /* epsilon */ { $$ = nullptr; }
    ;

order_clause:
      col  opt_asc_desc
    {
        $$ = std::make_shared<OrderBy>($1, $2);
    }
    |   order_clause ',' col opt_asc_desc
    {
        $1->append($3, $4);
        $$ = $1;
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
