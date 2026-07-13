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
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY
WHERE UPDATE SET TRANSACTION ISOLATION LEVEL SNAPSHOT SERIALIZABLE SELECT INT CHAR FLOAT INDEX AND JOIN ON AS EXPLAIN ANALYZE EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE
%token GROUP HAVING LIMIT COUNT MAX MIN SUM AVG
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING VALUE_FLOAT
%token <sv_int> VALUE_INT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt selectStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr aggExpr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName optAlias
%type <sv_strs> colNameList
%type <sv_table_ref> tableRef
%type <sv_from_clause> tableList
%type <sv_col> col
%type <sv_cols> colList opt_group_clause group_clause
%type <sv_select_item> selectItem
%type <sv_select_items> selector selectList
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause opt_having_clause
%type <sv_orderby>  order_item
%type <sv_orderbys> order_clause order_list opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_setKnobType> set_knob_type
%type <sv_int> opt_limit_clause

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   EXPLAIN ANALYZE selectStmt ';'
    {
        parse_tree = std::make_shared<ExplainAnalyze>(std::dynamic_pointer_cast<SelectStmt>($3));
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
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION
    {
        $$ = std::make_shared<SetIsolationStmt>(IsolationLevel::REPEATABLE_READ);
    }
    |   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
    {
        $$ = std::make_shared<SetIsolationStmt>(IsolationLevel::SERIALIZABLE);
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
    |   selectStmt
    ;

selectStmt:
        SELECT selector FROM tableList optWhereClause opt_group_clause opt_having_clause opt_order_clause opt_limit_clause
    {
        auto conds = $4->conds;
        conds.insert(conds.end(), $5.begin(), $5.end());
        $$ = std::make_shared<SelectStmt>($2, $4->table_refs, conds, $6, $7, $8, $9, $4->has_explicit_join);
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
        $$ = std::make_shared<FloatLit>(std::stof($1), $1);
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
        expr op expr
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
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
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
    |   aggExpr
    ;

aggExpr:
        COUNT '(' '*' ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_COUNT, nullptr, true);
    }
    |   COUNT '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_COUNT, $3);
    }
    |   MAX '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_MAX, $3);
    }
    |   MIN '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_MIN, $3);
    }
    |   SUM '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_SUM, $3);
    }
    |   AVG '(' col ')'
    {
        $$ = std::make_shared<AggFunc>(AGG_AVG, $3);
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
        $$ = std::make_shared<SetClause>($1, $3, '+', $5);
    }
    |   colName '=' colName VALUE_INT
    {
        $$ = std::make_shared<SetClause>($1, $3, '+', std::make_shared<IntLit>($4));
    }
    |   colName '=' colName VALUE_FLOAT
    {
        $$ = std::make_shared<SetClause>($1, $3, '+', std::make_shared<FloatLit>(std::stof($4), $4));
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   selectList
    ;

selectList:
        selectItem
    {
        $$ = std::vector<std::shared_ptr<SelectItem>>{$1};
    }
    |   selectList ',' selectItem
    {
        $$.push_back($3);
    }
    ;

selectItem:
        expr optAlias
    {
        $$ = std::make_shared<SelectItem>($1, $2);
    }
    ;

opt_group_clause:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<Col>>(); }
    |   GROUP BY group_clause
    {
        $$ = $3;
    }
    ;

group_clause:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   group_clause ',' col
    {
        $$.push_back($3);
    }
    ;

opt_having_clause:
        /* epsilon */ { $$ = std::vector<std::shared_ptr<BinaryExpr>>(); }
    |   HAVING whereClause
    {
        $$ = $2;
    }
    ;

tableList:
        tableRef
    {
        $$ = std::make_shared<FromClause>(std::vector<std::shared_ptr<TableRef>>{$1},
                                          std::vector<std::shared_ptr<BinaryExpr>>());
    }
    |   tableList ',' tableRef
    {
        $1->table_refs.push_back($3);
        $$ = $1;
    }
    |   tableList JOIN tableRef ON whereClause
    {
        $1->table_refs.push_back($3);
        $1->conds.insert($1->conds.end(), $5.begin(), $5.end());
        $1->has_explicit_join = true;
        $$ = $1;
    }
    |   tableList JOIN tableRef
    {
        $1->table_refs.push_back($3);
        $1->has_explicit_join = true;
        $$ = $1;
    }
    ;

tableRef:
        tbName optAlias
    {
        $$ = std::make_shared<TableRef>($1, $2);
    }
    ;

optAlias:
        /* epsilon */ { $$ = ""; }
    |   IDENTIFIER
    {
        $$ = $1;
    }
    |   AS IDENTIFIER
    {
        $$ = $2;
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ { $$ = std::vector<std::shared_ptr<OrderBy>>(); }
    ;

order_clause:
      order_list
    {
        $$ = $1;
    }
    ;

order_list:
      order_item
    {
        $$ = std::vector<std::shared_ptr<OrderBy>>{$1};
    }
    | order_list ',' order_item
    {
        $$.push_back($3);
    }
    ;

order_item:
      col  opt_asc_desc 
    { 
        $$ = std::make_shared<OrderBy>($1, $2);
    }
    ;   

opt_limit_clause:
      LIMIT VALUE_INT { $$ = $2; }
    | /* epsilon */ { $$ = -1; }
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
