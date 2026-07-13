#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct yy_buffer_state {
    std::string input;
};

namespace {

YY_BUFFER_STATE current_buffer = nullptr;

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return text;
}

enum class TokenKind {
    End,
    Word,
    Int,
    Float,
    String,
    LParen,
    RParen,
    Comma,
    Semicolon,
    Star,
    Dot,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Plus,
    Minus
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::string text;
    size_t pos{0};
};

class Lexer {
   public:
    explicit Lexer(std::string input) : input_(std::move(input)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (true) {
            skip_ignored();
            if (pos_ >= input_.size()) {
                tokens.push_back({TokenKind::End, "", pos_});
                return tokens;
            }

            const size_t start = pos_;
            const char ch = input_[pos_];
            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                ++pos_;
                while (pos_ < input_.size()) {
                    const unsigned char c = static_cast<unsigned char>(input_[pos_]);
                    if (!std::isalnum(c) && input_[pos_] != '_') break;
                    ++pos_;
                }
                tokens.push_back({TokenKind::Word, input_.substr(start, pos_ - start), start});
                continue;
            }

            if (ch == '\'' ) {
                ++pos_;
                std::string value;
                bool closed = false;
                while (pos_ < input_.size()) {
                    if (input_[pos_] == '\'') {
                        if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '\'') {
                            value.push_back('\'');
                            pos_ += 2;
                            continue;
                        }
                        ++pos_;
                        closed = true;
                        break;
                    }
                    value.push_back(input_[pos_++]);
                }
                if (!closed) throw std::runtime_error("unterminated string literal");
                tokens.push_back({TokenKind::String, std::move(value), start});
                continue;
            }

            const bool signed_number = (ch == '+' || ch == '-') &&
                pos_ + 1 < input_.size() &&
                (std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])) ||
                 input_[pos_ + 1] == '.');
            const bool unsigned_number = std::isdigit(static_cast<unsigned char>(ch)) ||
                (ch == '.' && pos_ + 1 < input_.size() &&
                 std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])));
            if (signed_number || unsigned_number) {
                if (ch == '+' || ch == '-') ++pos_;
                bool has_digit = false;
                while (pos_ < input_.size() &&
                       std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                    has_digit = true;
                    ++pos_;
                }
                bool is_float = false;
                if (pos_ < input_.size() && input_[pos_] == '.') {
                    is_float = true;
                    ++pos_;
                    while (pos_ < input_.size() &&
                           std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                        has_digit = true;
                        ++pos_;
                    }
                }
                if (!has_digit) throw std::runtime_error("invalid numeric literal");
                if (pos_ < input_.size() &&
                    (input_[pos_] == 'e' || input_[pos_] == 'E')) {
                    is_float = true;
                    size_t exp_pos = pos_++;
                    if (pos_ < input_.size() &&
                        (input_[pos_] == '+' || input_[pos_] == '-')) {
                        ++pos_;
                    }
                    const size_t exp_digits = pos_;
                    while (pos_ < input_.size() &&
                           std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                        ++pos_;
                    }
                    if (pos_ == exp_digits) pos_ = exp_pos;
                }
                tokens.push_back({is_float ? TokenKind::Float : TokenKind::Int,
                                  input_.substr(start, pos_ - start), start});
                continue;
            }

            ++pos_;
            switch (ch) {
                case '(': tokens.push_back({TokenKind::LParen, "(", start}); break;
                case ')': tokens.push_back({TokenKind::RParen, ")", start}); break;
                case ',': tokens.push_back({TokenKind::Comma, ",", start}); break;
                case ';': tokens.push_back({TokenKind::Semicolon, ";", start}); break;
                case '*': tokens.push_back({TokenKind::Star, "*", start}); break;
                case '.': tokens.push_back({TokenKind::Dot, ".", start}); break;
                case '+': tokens.push_back({TokenKind::Plus, "+", start}); break;
                case '-': tokens.push_back({TokenKind::Minus, "-", start}); break;
                case '=': tokens.push_back({TokenKind::Eq, "=", start}); break;
                case '<':
                    if (pos_ < input_.size() && input_[pos_] == '=') {
                        ++pos_;
                        tokens.push_back({TokenKind::Le, "<=", start});
                    } else if (pos_ < input_.size() && input_[pos_] == '>') {
                        ++pos_;
                        tokens.push_back({TokenKind::Ne, "<>", start});
                    } else {
                        tokens.push_back({TokenKind::Lt, "<", start});
                    }
                    break;
                case '>':
                    if (pos_ < input_.size() && input_[pos_] == '=') {
                        ++pos_;
                        tokens.push_back({TokenKind::Ge, ">=", start});
                    } else {
                        tokens.push_back({TokenKind::Gt, ">", start});
                    }
                    break;
                case '!':
                    if (pos_ < input_.size() && input_[pos_] == '=') {
                        ++pos_;
                        tokens.push_back({TokenKind::Ne, "!=", start});
                    } else {
                        throw std::runtime_error("unexpected '!'");
                    }
                    break;
                default:
                    throw std::runtime_error(std::string("unexpected character '") + ch + "'");
            }
        }
    }

   private:
    void skip_ignored() {
        while (pos_ < input_.size()) {
            // Some SQL fixtures are UTF-8 files with a byte-order mark.  A BOM
            // is not part of the SQL statement and must not become a lexer
            // error on the first keyword.
            if (input_.compare(pos_, 3, "\xEF\xBB\xBF") == 0) {
                pos_ += 3;
                continue;
            }
            // Treat UTF-8 non-breaking space like ordinary SQL whitespace.
            if (input_.compare(pos_, 2, "\xC2\xA0") == 0) {
                pos_ += 2;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(input_[pos_]))) {
                ++pos_;
                continue;
            }
            if (input_[pos_] == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
                pos_ += 2;
                while (pos_ < input_.size() && input_[pos_] != '\n' && input_[pos_] != '\r') ++pos_;
                continue;
            }
            if (input_[pos_] == '/' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '*') {
                pos_ += 2;
                const size_t end = input_.find("*/", pos_);
                if (end == std::string::npos) throw std::runtime_error("unterminated block comment");
                pos_ = end + 2;
                continue;
            }
            break;
        }
    }

    std::string input_;
    size_t pos_{0};
};

class Parser {
   public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::shared_ptr<ast::TreeNode> parse() {
        if (peek().kind == TokenKind::End) return nullptr;
        auto result = parse_statement();
        while (consume_if(TokenKind::Semicolon)) {
        }
        expect(TokenKind::End, "end of statement");
        return result;
    }

   private:
    const Token &peek(size_t ahead = 0) const {
        const size_t index = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[index];
    }

    bool is_kw(const Token &token, const char *keyword) const {
        return token.kind == TokenKind::Word && upper(token.text) == keyword;
    }

    bool peek_kw(const char *keyword, size_t ahead = 0) const {
        return is_kw(peek(ahead), keyword);
    }

    Token consume() { return tokens_[pos_++]; }

    bool consume_if(TokenKind kind) {
        if (peek().kind != kind) return false;
        ++pos_;
        return true;
    }

    bool consume_kw(const char *keyword) {
        if (!peek_kw(keyword)) return false;
        ++pos_;
        return true;
    }

    Token expect(TokenKind kind, const char *description) {
        if (peek().kind != kind) fail(std::string("expected ") + description);
        return consume();
    }

    void expect_kw(const char *keyword) {
        if (!consume_kw(keyword)) fail(std::string("expected ") + keyword);
    }

    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error(message + " near position " + std::to_string(peek().pos));
    }

    std::string parse_name() {
        return expect(TokenKind::Word, "identifier").text;
    }

    bool is_clause_keyword(const Token &token) const {
        if (token.kind != TokenKind::Word) return false;
        static const std::unordered_set<std::string> reserved = {
            "FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "JOIN", "ON",
            "UNION", "AND", "ASC", "DESC", "AS", "SET", "VALUES"
        };
        return reserved.count(upper(token.text)) != 0;
    }

    std::shared_ptr<ast::TreeNode> parse_statement() {
        if (consume_kw("HELP")) return std::make_shared<ast::Help>();
        if (consume_kw("EXIT")) return nullptr;
        if (consume_kw("BEGIN")) return std::make_shared<ast::TxnBegin>();
        if (consume_kw("COMMIT")) return std::make_shared<ast::TxnCommit>();
        if (consume_kw("ABORT")) return std::make_shared<ast::TxnAbort>();
        if (consume_kw("ROLLBACK")) return std::make_shared<ast::TxnRollback>();
        if (consume_kw("SHOW")) return parse_show();
        if (consume_kw("CREATE")) return parse_create();
        if (consume_kw("DROP")) return parse_drop();
        if (consume_kw("DESC")) return std::make_shared<ast::DescTable>(parse_name());
        if (consume_kw("INSERT")) return parse_insert();
        if (consume_kw("DELETE")) return parse_delete();
        if (consume_kw("UPDATE")) return parse_update();
        if (consume_kw("SET")) return parse_set();
        if (consume_kw("SELECT")) return parse_select(false);
        if (consume_kw("EXPLAIN")) {
            expect_kw("ANALYZE");
            expect_kw("SELECT");
            return parse_select(true);
        }
        fail("unsupported statement");
    }

    std::shared_ptr<ast::TreeNode> parse_show() {
        if (consume_kw("TABLES")) return std::make_shared<ast::ShowTables>();
        if (consume_kw("INDEX")) {
            expect_kw("FROM");
            return std::make_shared<ast::ShowIndex>(parse_name());
        }
        fail("expected TABLES or INDEX after SHOW");
    }

    std::shared_ptr<ast::TreeNode> parse_create() {
        if (consume_kw("STATIC_CHECKPOINT")) {
            return std::make_shared<ast::StaticCheckpoint>();
        }
        if (consume_kw("TABLE")) {
            const std::string table = parse_name();
            expect(TokenKind::LParen, "'('");
            std::vector<std::shared_ptr<ast::Field>> fields;
            do {
                const std::string name = parse_name();
                std::shared_ptr<ast::TypeLen> type;
                if (consume_kw("INT")) {
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_INT, sizeof(int));
                } else if (consume_kw("FLOAT")) {
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_FLOAT, sizeof(float));
                } else if (consume_kw("CHAR")) {
                    expect(TokenKind::LParen, "'('");
                    const Token length = expect(TokenKind::Int, "character length");
                    expect(TokenKind::RParen, "')'");
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_STRING, std::stoi(length.text));
                } else if (consume_kw("DATETIME") || consume_kw("TIMESTAMP")) {
                    // The transaction benchmark uses valid ISO-style values.
                    // Store them as fixed-width character data so comparison
                    // and unique-index ordering remain chronological.
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_STRING, 19);
                } else if (consume_kw("DATE")) {
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_STRING, 10);
                } else if (consume_kw("TIME")) {
                    type = std::make_shared<ast::TypeLen>(ast::SV_TYPE_STRING, 8);
                } else {
                    fail("expected column type");
                }
                fields.push_back(std::make_shared<ast::ColDef>(name, std::move(type)));
            } while (consume_if(TokenKind::Comma));
            expect(TokenKind::RParen, "')'");
            return std::make_shared<ast::CreateTable>(table, std::move(fields));
        }
        if (consume_kw("INDEX")) {
            const std::string table = parse_name();
            return std::make_shared<ast::CreateIndex>(table, parse_name_list());
        }
        fail("expected TABLE, INDEX, or STATIC_CHECKPOINT after CREATE");
    }

    std::shared_ptr<ast::TreeNode> parse_drop() {
        if (consume_kw("TABLE")) return std::make_shared<ast::DropTable>(parse_name());
        if (consume_kw("INDEX")) {
            const std::string table = parse_name();
            return std::make_shared<ast::DropIndex>(table, parse_name_list());
        }
        fail("expected TABLE or INDEX after DROP");
    }

    std::vector<std::string> parse_name_list() {
        expect(TokenKind::LParen, "'('");
        std::vector<std::string> names;
        do {
            names.push_back(parse_name());
        } while (consume_if(TokenKind::Comma));
        expect(TokenKind::RParen, "')'");
        return names;
    }

    std::shared_ptr<ast::Value> parse_value() {
        const Token token = peek();
        if (token.kind == TokenKind::Int) {
            consume();
            return std::make_shared<ast::IntLit>(std::stoi(token.text), token.text);
        }
        if (token.kind == TokenKind::Float) {
            consume();
            return std::make_shared<ast::FloatLit>(std::stof(token.text), token.text);
        }
        if (token.kind == TokenKind::String) {
            consume();
            return std::make_shared<ast::StringLit>(token.text);
        }
        if (consume_kw("TRUE")) return std::make_shared<ast::BoolLit>(true);
        if (consume_kw("FALSE")) return std::make_shared<ast::BoolLit>(false);
        fail("expected literal value");
    }

    std::shared_ptr<ast::TreeNode> parse_insert() {
        expect_kw("INTO");
        const std::string table = parse_name();
        expect_kw("VALUES");
        expect(TokenKind::LParen, "'('");
        std::vector<std::shared_ptr<ast::Value>> values;
        do {
            values.push_back(parse_value());
        } while (consume_if(TokenKind::Comma));
        expect(TokenKind::RParen, "')'");
        return std::make_shared<ast::InsertStmt>(table, std::move(values));
    }

    ast::SvCompOp parse_comp_op() {
        switch (consume().kind) {
            case TokenKind::Eq: return ast::SV_OP_EQ;
            case TokenKind::Ne: return ast::SV_OP_NE;
            case TokenKind::Lt: return ast::SV_OP_LT;
            case TokenKind::Gt: return ast::SV_OP_GT;
            case TokenKind::Le: return ast::SV_OP_LE;
            case TokenKind::Ge: return ast::SV_OP_GE;
            default: fail("expected comparison operator");
        }
    }

    bool is_aggregate_name(const Token &token) const {
        if (token.kind != TokenKind::Word || peek(1).kind != TokenKind::LParen) return false;
        const std::string name = upper(token.text);
        return name == "COUNT" || name == "MAX" || name == "MIN" ||
               name == "SUM" || name == "AVG";
    }

    std::shared_ptr<ast::Col> parse_col() {
        const std::string first = parse_name();
        if (consume_if(TokenKind::Dot)) {
            return std::make_shared<ast::Col>(first, parse_name());
        }
        return std::make_shared<ast::Col>("", first);
    }

    std::shared_ptr<ast::AggExpr> parse_aggregate() {
        const std::string name = upper(parse_name());
        ast::AggType type;
        if (name == "COUNT") type = ast::AGG_COUNT;
        else if (name == "MAX") type = ast::AGG_MAX;
        else if (name == "MIN") type = ast::AGG_MIN;
        else if (name == "SUM") type = ast::AGG_SUM;
        else if (name == "AVG") type = ast::AGG_AVG;
        else fail("unknown aggregate function");
        expect(TokenKind::LParen, "'('");
        if (consume_if(TokenKind::Star)) {
            expect(TokenKind::RParen, "')'");
            return std::make_shared<ast::AggExpr>(type, true);
        }
        auto col = parse_col();
        expect(TokenKind::RParen, "')'");
        return std::make_shared<ast::AggExpr>(type, false, std::move(col));
    }

    std::shared_ptr<ast::Expr> parse_nonvalue_expr() {
        if (is_aggregate_name(peek())) return parse_aggregate();
        return parse_col();
    }

    std::shared_ptr<ast::Expr> parse_rhs_expr() {
        if (peek().kind == TokenKind::Int || peek().kind == TokenKind::Float ||
            peek().kind == TokenKind::String || peek_kw("TRUE") || peek_kw("FALSE")) {
            return parse_value();
        }
        return parse_nonvalue_expr();
    }

    std::shared_ptr<ast::BinaryExpr> parse_condition() {
        auto lhs = parse_nonvalue_expr();
        const ast::SvCompOp op = parse_comp_op();
        auto rhs = parse_rhs_expr();
        return std::make_shared<ast::BinaryExpr>(std::move(lhs), op, std::move(rhs));
    }

    std::vector<std::shared_ptr<ast::BinaryExpr>> parse_condition_list() {
        std::vector<std::shared_ptr<ast::BinaryExpr>> conditions;
        conditions.push_back(parse_condition());
        while (consume_kw("AND")) conditions.push_back(parse_condition());
        return conditions;
    }

    std::shared_ptr<ast::TreeNode> parse_delete() {
        expect_kw("FROM");
        const std::string table = parse_name();
        std::vector<std::shared_ptr<ast::BinaryExpr>> conditions;
        if (consume_kw("WHERE")) conditions = parse_condition_list();
        return std::make_shared<ast::DeleteStmt>(table, std::move(conditions));
    }

    std::shared_ptr<ast::TreeNode> parse_update() {
        const std::string table = parse_name();
        expect_kw("SET");
        std::vector<std::shared_ptr<ast::SetClause>> clauses;
        do {
            const std::string name = parse_name();
            expect(TokenKind::Eq, "'='");
            if (peek().kind == TokenKind::Word) {
                const std::string source = parse_name();
                std::shared_ptr<ast::Value> delta;
                if (peek().kind == TokenKind::Int || peek().kind == TokenKind::Float) {
                    const std::string text = peek().text;
                    if (text.empty() || (text.front() != '+' && text.front() != '-')) {
                        fail("self-referential UPDATE requires + or - delta");
                    }
                    delta = parse_value();
                } else {
                    bool negative = false;
                    if (consume_if(TokenKind::Plus)) {
                        negative = false;
                    } else if (consume_if(TokenKind::Minus)) {
                        negative = true;
                    } else {
                        fail("self-referential UPDATE requires + or - delta");
                    }
                    delta = parse_value();
                    if (negative) {
                        if (auto value = std::dynamic_pointer_cast<ast::IntLit>(delta)) {
                            value->val = -value->val;
                            value->display = "-" + value->display;
                        } else if (auto value = std::dynamic_pointer_cast<ast::FloatLit>(delta)) {
                            value->val = -value->val;
                            value->display = "-" + value->display;
                        } else {
                            fail("UPDATE delta must be numeric");
                        }
                    }
                }
                clauses.push_back(std::make_shared<ast::SetClause>(name, source, std::move(delta)));
            } else {
                clauses.push_back(std::make_shared<ast::SetClause>(name, parse_value()));
            }
        } while (consume_if(TokenKind::Comma));
        std::vector<std::shared_ptr<ast::BinaryExpr>> conditions;
        if (consume_kw("WHERE")) conditions = parse_condition_list();
        return std::make_shared<ast::UpdateStmt>(table, std::move(clauses), std::move(conditions));
    }

    std::shared_ptr<ast::TreeNode> parse_set() {
        // Accept the competition syntax and the standard session-scoped forms
        // that a syntax-only driver may emit.  All accepted spellings map to
        // the same two session isolation levels.
        bool scoped = false;
        if (consume_kw("SESSION") || consume_kw("LOCAL")) scoped = true;
        if (scoped && consume_kw("CHARACTERISTICS")) {
            expect_kw("AS");
        }

        if (consume_kw("TRANSACTION")) {
            expect_kw("ISOLATION");
            expect_kw("LEVEL");
            if (consume_kw("SNAPSHOT_ISOLATION") || consume_kw("SI")) {
                return std::make_shared<ast::SetIsolation>(ast::IsolationSnapshot);
            }
            if (consume_kw("SERIALIZABLE_ISOLATION") || consume_kw("SER")) {
                return std::make_shared<ast::SetIsolation>(ast::IsolationSerializable);
            }
            if (consume_kw("SNAPSHOT")) {
                // The statement required by the competition contains the
                // second ISOLATION keyword.  Also accept SQL Server-style
                // "... LEVEL SNAPSHOT" for parser compatibility.
                consume_kw("ISOLATION");
                return std::make_shared<ast::SetIsolation>(ast::IsolationSnapshot);
            }
            if (consume_kw("SERIALIZABLE")) {
                // Be tolerant of the symmetric phrase SERIALIZABLE ISOLATION.
                consume_kw("ISOLATION");
                return std::make_shared<ast::SetIsolation>(ast::IsolationSerializable);
            }
            fail("expected SNAPSHOT ISOLATION or SERIALIZABLE");
        }
        if (scoped) fail("expected TRANSACTION after SET SESSION/LOCAL");
        ast::SetKnobType type;
        if (consume_kw("ENABLE_NESTLOOP")) type = ast::EnableNestLoop;
        else if (consume_kw("ENABLE_SORTMERGE")) type = ast::EnableSortMerge;
        else fail("unknown SET option");
        expect(TokenKind::Eq, "'='");
        bool value;
        if (consume_kw("TRUE")) value = true;
        else if (consume_kw("FALSE")) value = false;
        else fail("expected TRUE or FALSE");
        return std::make_shared<ast::SetStmt>(type, value);
    }

    ast::TableRef parse_table_ref() {
        const std::string table = parse_name();
        std::string alias;
        if (consume_kw("AS")) {
            alias = parse_name();
        } else if (peek().kind == TokenKind::Word && !is_clause_keyword(peek())) {
            alias = consume().text;
        }
        return ast::TableRef(table, alias);
    }

    std::shared_ptr<ast::FromClause> parse_from_clause() {
        auto result = std::make_shared<ast::FromClause>();
        result->tables.push_back(parse_table_ref());
        while (true) {
            if (consume_if(TokenKind::Comma)) {
                result->tables.push_back(parse_table_ref());
                continue;
            }
            if (consume_kw("JOIN")) {
                result->tables.push_back(parse_table_ref());
                if (consume_kw("ON")) {
                    auto conditions = parse_condition_list();
                    result->conds.insert(result->conds.end(), conditions.begin(), conditions.end());
                }
                continue;
            }
            break;
        }
        return result;
    }

    std::shared_ptr<ast::SelectItem> parse_select_item() {
        auto expr = parse_nonvalue_expr();
        std::string alias;
        if (consume_kw("AS")) {
            alias = parse_name();
        } else if (peek().kind == TokenKind::Word && !is_clause_keyword(peek())) {
            alias = consume().text;
        }
        return std::make_shared<ast::SelectItem>(std::move(expr), std::move(alias));
    }

    std::shared_ptr<ast::TreeNode> parse_select(bool explain) {
        bool select_all = false;
        std::vector<std::shared_ptr<ast::SelectItem>> items;
        if (consume_if(TokenKind::Star)) {
            select_all = true;
        } else {
            do {
                items.push_back(parse_select_item());
            } while (consume_if(TokenKind::Comma));
        }

        expect_kw("FROM");

        // The title-6 grammar only needs a UNION-derived table as the sole
        // source of an outer SELECT *. Parse it here before the regular table
        // reference path so every UNION branch can reuse the complete SELECT
        // parser (WHERE/GROUP BY/HAVING/ORDER BY/LIMIT included).
        if (select_all && consume_if(TokenKind::LParen)) {
            std::vector<std::shared_ptr<ast::SelectStmt>> branches;
            do {
                expect_kw("SELECT");
                auto branch_node = parse_select(false);
                auto branch = std::dynamic_pointer_cast<ast::SelectStmt>(branch_node);
                if (!branch) fail("UNION branch must be a SELECT query");
                branches.push_back(std::move(branch));
            } while (consume_kw("UNION"));
            expect(TokenKind::RParen, "')'");
            if (branches.size() < 2) fail("derived table must contain UNION");

            std::string alias;
            if (consume_kw("AS")) {
                alias = parse_name();
            } else if (peek().kind == TokenKind::Word && !is_clause_keyword(peek())) {
                alias = consume().text;
            } else {
                fail("UNION-derived table requires an alias");
            }

            std::vector<std::shared_ptr<ast::OrderBy>> outer_orders;
            if (consume_kw("ORDER")) {
                expect_kw("BY");
                do {
                    auto expr = parse_nonvalue_expr();
                    ast::OrderByDir dir = ast::OrderBy_DEFAULT;
                    if (consume_kw("ASC")) dir = ast::OrderBy_ASC;
                    else if (consume_kw("DESC")) dir = ast::OrderBy_DESC;
                    outer_orders.push_back(std::make_shared<ast::OrderBy>(std::move(expr), dir));
                } while (consume_if(TokenKind::Comma));
            }

            int outer_limit = -1;
            if (consume_kw("LIMIT")) {
                const Token value = expect(TokenKind::Int, "non-negative LIMIT value");
                outer_limit = std::stoi(value.text);
                if (outer_limit < 0) fail("LIMIT must be non-negative");
            }
            return std::make_shared<ast::UnionStmt>(
                std::move(branches), std::move(alias), std::move(outer_orders),
                outer_limit, explain);
        }

        auto from = parse_from_clause();
        std::vector<std::shared_ptr<ast::BinaryExpr>> conditions = from->conds;
        if (consume_kw("WHERE")) {
            auto where = parse_condition_list();
            conditions.insert(conditions.end(), where.begin(), where.end());
        }

        std::vector<std::shared_ptr<ast::Col>> group_by;
        if (consume_kw("GROUP")) {
            expect_kw("BY");
            do {
                group_by.push_back(parse_col());
            } while (consume_if(TokenKind::Comma));
        }

        std::vector<std::shared_ptr<ast::BinaryExpr>> having;
        if (consume_kw("HAVING")) having = parse_condition_list();

        std::vector<std::shared_ptr<ast::OrderBy>> orders;
        if (consume_kw("ORDER")) {
            expect_kw("BY");
            do {
                auto expr = parse_nonvalue_expr();
                ast::OrderByDir dir = ast::OrderBy_DEFAULT;
                if (consume_kw("ASC")) dir = ast::OrderBy_ASC;
                else if (consume_kw("DESC")) dir = ast::OrderBy_DESC;
                orders.push_back(std::make_shared<ast::OrderBy>(std::move(expr), dir));
            } while (consume_if(TokenKind::Comma));
        }

        int limit = -1;
        if (consume_kw("LIMIT")) {
            const Token value = expect(TokenKind::Int, "non-negative LIMIT value");
            limit = std::stoi(value.text);
            if (limit < 0) fail("LIMIT must be non-negative");
        }

        return std::make_shared<ast::SelectStmt>(
            select_all, std::move(items), std::move(from->tables),
            std::move(conditions), std::move(group_by), std::move(having),
            std::move(orders), limit, explain);
    }

    std::vector<Token> tokens_;
    size_t pos_{0};
};

}  // namespace

YY_BUFFER_STATE yy_scan_string(const char *str) {
    auto *buffer = new yy_buffer_state();
    buffer->input = str == nullptr ? "" : str;
    current_buffer = buffer;
    return buffer;
}

void yy_delete_buffer(YY_BUFFER_STATE buffer) {
    if (current_buffer == buffer) current_buffer = nullptr;
    delete buffer;
}

int yyparse() {
    ast::parse_tree.reset();
    if (current_buffer == nullptr) return 1;
    try {
        Lexer lexer(current_buffer->input);
        Parser parser(lexer.tokenize());
        ast::parse_tree = parser.parse();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Parser Error: " << error.what() << std::endl;
        ast::parse_tree.reset();
        return 1;
    }
}
