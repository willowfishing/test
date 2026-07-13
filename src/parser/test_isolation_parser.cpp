/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#undef NDEBUG

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "parser.h"

namespace {

void expect_isolation(const std::string &sql, ast::IsolationChoice expected) {
    ast::parse_tree.reset();
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    const int status = yyparse();
    yy_delete_buffer(buffer);

    assert(status == 0);
    auto statement = std::dynamic_pointer_cast<ast::SetIsolation>(ast::parse_tree);
    assert(statement != nullptr);
    assert(statement->level == expected);
}

void expect_self_referential_update(const std::string &sql) {
    ast::parse_tree.reset();
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    const int status = yyparse();
    yy_delete_buffer(buffer);

    assert(status == 0);
    auto statement = std::dynamic_pointer_cast<ast::UpdateStmt>(ast::parse_tree);
    assert(statement != nullptr);
    assert(statement->set_clauses.size() == 1);
    assert(statement->set_clauses.front()->self_ref);
    assert(statement->set_clauses.front()->col_name == "col_score");
    assert(statement->set_clauses.front()->source_col == "col_score");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, ast::IsolationChoice>> cases = {
        {"SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;", ast::IsolationSerializable},
        {"set transaction isolation level snapshot isolation;", ast::IsolationSnapshot},
        {"set transaction isolation level serializable;", ast::IsolationSerializable},
        {"SeT TrAnSaCtIoN IsOlAtIoN LeVeL SnApShOt IsOlAtIoN;", ast::IsolationSnapshot},
        {"SET\tTRANSACTION\nISOLATION LEVEL SNAPSHOT ISOLATION ;", ast::IsolationSnapshot},
        {"/* before */ SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;", ast::IsolationSerializable},
        {"-- before\nSET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", ast::IsolationSerializable},
        {"SET TRANSACTION ISOLATION LEVEL SNAPSHOT;", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE ISOLATION;", ast::IsolationSerializable},
        {"SET SESSION TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;", ast::IsolationSnapshot},
        {"SET LOCAL TRANSACTION ISOLATION LEVEL SERIALIZABLE;", ast::IsolationSerializable},
        {"SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE;", ast::IsolationSerializable},
        {"SET TRANSACTION ISOLATION LEVEL SNAPSHOT_ISOLATION;", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE_ISOLATION;", ast::IsolationSerializable},
        {"SET TRANSACTION ISOLATION LEVEL SI;", ast::IsolationSnapshot},
        {"SET TRANSACTION ISOLATION LEVEL SER;", ast::IsolationSerializable},
        {"SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;;;", ast::IsolationSnapshot},
        {std::string("\xEF\xBB\xBF") + "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;", ast::IsolationSnapshot},
        {std::string("SET") + "\xC2\xA0" + "TRANSACTION ISOLATION LEVEL SERIALIZABLE;", ast::IsolationSerializable},
    };

    for (const auto &[sql, expected] : cases) expect_isolation(sql, expected);
    expect_self_referential_update(
        "UPDATE tab SET col_score=col_score+5 where col_id<3;");

    ast::parse_tree.reset();
    return 0;
}
