#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "parser/ast.h"

namespace rmdb {

enum class SqlTemplateLiteralType { kInt, kFloat, kString };

struct SqlTemplateLiteral {
    SqlTemplateLiteralType type = SqlTemplateLiteralType::kInt;
    int int_val = 0;
    float float_val = 0.0f;
    std::string str_val;
};

struct SqlTemplateKey {
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    uint32_t normalized_len = 0;
    uint16_t literal_count = 0;

    bool operator==(const SqlTemplateKey &other) const {
        return h1 == other.h1 && h2 == other.h2 && normalized_len == other.normalized_len &&
               literal_count == other.literal_count;
    }
};

struct SqlTemplateKeyHash {
    size_t operator()(const SqlTemplateKey &key) const {
        uint64_t mixed = key.h1 ^ (key.h2 + 0x9e3779b97f4a7c15ULL + (key.h1 << 6) + (key.h1 >> 2));
        mixed ^= static_cast<uint64_t>(key.normalized_len) << 32;
        mixed ^= key.literal_count;
        return static_cast<size_t>(mixed);
    }
};

class SqlTemplateLiteralList {
   public:
    static constexpr size_t kInlineCapacity = 16;

    bool push_back(SqlTemplateLiteral literal) {
        if (size_ == std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        size_t index = size_++;
        if (index < kInlineCapacity) {
            inline_literals_[index] = std::move(literal);
        } else {
            overflow_literals_.push_back(std::move(literal));
        }
        return true;
    }

    size_t size() const { return size_; }

    const SqlTemplateLiteral &operator[](size_t index) const {
        return index < kInlineCapacity ? inline_literals_[index] : overflow_literals_[index - kInlineCapacity];
    }

   private:
    uint16_t size_ = 0;
    std::array<SqlTemplateLiteral, kInlineCapacity> inline_literals_{};
    std::vector<SqlTemplateLiteral> overflow_literals_;
};

struct SqlTemplateCandidate {
    SqlTemplateKey key;
    SqlTemplateLiteralList literals;
};

namespace sql_template_detail {

enum class SlotKind { kValue, kLimitInt };

struct LiteralSlot {
    SlotKind kind = SlotKind::kValue;
    SqlTemplateLiteralType type = SqlTemplateLiteralType::kInt;
    int numeric_sign = 1;
};

inline std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

inline std::string upper_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return text;
}

inline bool is_keyword(const std::string &ident) {
    static const std::unordered_set<std::string> keywords = {
        "SHOW",       "TABLES",    "CREATE",   "TABLE",      "DROP",      "DESC",
        "INSERT",     "INTO",      "VALUES",   "DELETE",     "FROM",      "WHERE",
        "UPDATE",     "SET",       "TRANSACTION", "ISOLATION", "LEVEL",     "SNAPSHOT",
        "SERIALIZABLE", "SELECT",  "INT",      "CHAR",       "FLOAT",     "DATETIME",
        "INDEX",      "AND",       "JOIN",     "SEMI",       "ON",        "GROUP",
        "HAVING",     "LIMIT",     "AS",       "EXPLAIN",    "ANALYZE",   "UNION",
        "EXIT",       "HELP",      "ORDER",    "BY",         "ASC",       "DESC",
        "ENABLE_NESTLOOP", "ENABLE_SORTMERGE", "STATIC_CHECKPOINT", "LOAD", "BEGIN",
        "COMMIT",     "ABORT",     "ROLLBACK", "TRUE",       "FALSE",     "MAX",
        "MIN",        "COUNT",     "SUM",      "AVG"};
    return keywords.count(upper_copy(ident)) != 0;
}

inline bool is_cacheable_start(const std::string &token) {
    return token == "select" || token == "insert" || token == "update" || token == "delete";
}

inline bool has_unsupported_keyword(const std::vector<std::string> &tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto &token = tokens[i];
        if (token == "set" && i + 1 < tokens.size() && tokens[i + 1] == "transaction") {
            return true;
        }
        if (token == "create" || token == "drop" || token == "load" || token == "desc" || token == "show" ||
            token == "begin" || token == "commit" || token == "abort" || token == "rollback" || token == "explain" ||
            token == "analyze" || token == "union" || token == "static_checkpoint" || token == "help" ||
            token == "exit" || token == "true" || token == "false") {
            return true;
        }
    }
    return false;
}

inline void append_token(std::vector<std::string> *tokens, std::string token) {
    if (!token.empty()) {
        tokens->push_back(std::move(token));
    }
}

inline std::string join_tokens(const std::vector<std::string> &tokens) {
    std::string key;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) {
            key.push_back(' ');
        }
        key += tokens[i];
    }
    return key;
}

inline bool read_string_literal(const std::string &sql, size_t *pos, std::string *value) {
    size_t i = *pos + 1;
    while (i < sql.size() && sql[i] != '\'') {
        ++i;
    }
    if (i >= sql.size()) {
        return false;
    }
    *value = sql.substr(*pos + 1, i - *pos - 1);
    *pos = i + 1;
    return true;
}

inline bool read_numeric_literal(const std::string &sql, size_t *pos, SqlTemplateLiteral *literal,
                                 std::string *placeholder) {
    size_t i = *pos;
    while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    bool is_float = false;
    if (i < sql.size() && sql[i] == '.') {
        is_float = true;
        ++i;
        while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) {
            ++i;
        }
    }
    std::string text = sql.substr(*pos, i - *pos);
    if (text.empty()) {
        return false;
    }
    if (is_float) {
        literal->type = SqlTemplateLiteralType::kFloat;
        literal->float_val = std::strtof(text.c_str(), nullptr);
        *placeholder = "?f";
    } else {
        literal->type = SqlTemplateLiteralType::kInt;
        literal->int_val = std::atoi(text.c_str());
        *placeholder = "?i";
    }
    *pos = i;
    return true;
}

inline void collect_value_slot(const std::shared_ptr<ast::Value> &value, std::vector<LiteralSlot> *slots) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kInt;
        slot.numeric_sign = int_lit->val < 0 ? -1 : 1;
        slots->push_back(slot);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kFloat;
        slot.numeric_sign = float_lit->val < 0.0f ? -1 : 1;
        slots->push_back(slot);
    } else if (std::dynamic_pointer_cast<ast::StringLit>(value)) {
        LiteralSlot slot;
        slot.type = SqlTemplateLiteralType::kString;
        slots->push_back(slot);
    }
}

inline bool has_bool_value(const std::shared_ptr<ast::Value> &value) {
    return std::dynamic_pointer_cast<ast::BoolLit>(value) != nullptr;
}

inline void collect_expr_slots(const std::shared_ptr<ast::Expr> &expr, std::vector<LiteralSlot> *slots) {
    if (auto value = std::dynamic_pointer_cast<ast::Value>(expr)) {
        collect_value_slot(value, slots);
    }
}

inline void collect_binary_slots(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds,
                                 std::vector<LiteralSlot> *slots) {
    for (const auto &cond : conds) {
        collect_expr_slots(cond->rhs, slots);
    }
}

inline void collect_having_slots(const std::vector<std::shared_ptr<ast::HavingExpr>> &conds,
                                 std::vector<LiteralSlot> *slots) {
    for (const auto &cond : conds) {
        collect_value_slot(cond->rhs, slots);
    }
}

inline void collect_select_slots(const std::shared_ptr<ast::SelectStmt> &select, std::vector<LiteralSlot> *slots) {
    collect_binary_slots(select->conds, slots);
    collect_having_slots(select->having_conds, slots);
    collect_binary_slots(select->semi_conds, slots);
    if (select->has_limit) {
        LiteralSlot slot;
        slot.kind = SlotKind::kLimitInt;
        slot.type = SqlTemplateLiteralType::kInt;
        slot.numeric_sign = select->limit < 0 ? -1 : 1;
        slots->push_back(slot);
    }
}

inline bool value_supported(const std::shared_ptr<ast::Value> &value) {
    return value != nullptr && !has_bool_value(value);
}

inline bool expr_supported(const std::shared_ptr<ast::Expr> &expr) {
    if (auto value = std::dynamic_pointer_cast<ast::Value>(expr)) {
        return value_supported(value);
    }
    return std::dynamic_pointer_cast<ast::Col>(expr) != nullptr;
}

inline bool binary_supported(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    for (const auto &cond : conds) {
        if (cond == nullptr || cond->lhs == nullptr || !expr_supported(cond->rhs)) {
            return false;
        }
    }
    return true;
}

inline bool having_supported(const std::vector<std::shared_ptr<ast::HavingExpr>> &conds) {
    for (const auto &cond : conds) {
        if (cond == nullptr || cond->lhs == nullptr || !value_supported(cond->rhs)) {
            return false;
        }
    }
    return true;
}

inline bool select_supported(const std::shared_ptr<ast::SelectStmt> &select) {
    if (select == nullptr || !binary_supported(select->conds) || !binary_supported(select->semi_conds) ||
        !having_supported(select->having_conds)) {
        return false;
    }
    for (const auto &ref : select->table_refs) {
        if (ref == nullptr || !ref->union_selects.empty()) {
            return false;
        }
    }
    return true;
}

inline bool root_supported(const std::shared_ptr<ast::TreeNode> &root) {
    if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
        return std::all_of(insert->vals.begin(), insert->vals.end(), value_supported);
    }
    if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
        if (!binary_supported(update->conds)) {
            return false;
        }
        for (const auto &set_clause : update->set_clauses) {
            if (set_clause == nullptr || (!set_clause->rhs_is_col && !value_supported(set_clause->val)) ||
                (set_clause->rhs_is_col && set_clause->op != ast::SET_OP_ASSIGN && !value_supported(set_clause->val))) {
                return false;
            }
        }
        return true;
    }
    if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
        return binary_supported(del->conds);
    }
    if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
        return select_supported(select);
    }
    return false;
}

inline void collect_root_slots(const std::shared_ptr<ast::TreeNode> &root, std::vector<LiteralSlot> *slots) {
    if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
        for (const auto &value : insert->vals) {
            collect_value_slot(value, slots);
        }
    } else if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
        for (const auto &set_clause : update->set_clauses) {
            if (!set_clause->rhs_is_col || set_clause->op != ast::SET_OP_ASSIGN) {
                collect_value_slot(set_clause->val, slots);
            }
        }
        collect_binary_slots(update->conds, slots);
    } else if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
        collect_binary_slots(del->conds, slots);
    } else if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
        collect_select_slots(select, slots);
    }
}

inline bool validate_slots(const std::vector<LiteralSlot> &slots, const SqlTemplateLiteralList &literals) {
    if (slots.size() != literals.size()) {
        return false;
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].type != literals[i].type) {
            return false;
        }
    }
    return true;
}

struct CloneContext {
    const SqlTemplateLiteralList *literals = nullptr;
    const std::vector<LiteralSlot> *slots = nullptr;
    size_t index = 0;
};

inline std::shared_ptr<ast::Col> clone_col(const std::shared_ptr<ast::Col> &col) {
    if (col == nullptr) {
        return nullptr;
    }
    return std::make_shared<ast::Col>(col->tab_name, col->col_name);
}

inline std::shared_ptr<ast::Value> clone_value(const std::shared_ptr<ast::Value> &value, CloneContext *ctx) {
    if (value == nullptr) {
        return nullptr;
    }
    if (ctx != nullptr && ctx->literals != nullptr && ctx->slots != nullptr) {
        const auto &slot = (*ctx->slots)[ctx->index];
        const auto &literal = (*ctx->literals)[ctx->index++];
        if (slot.type == SqlTemplateLiteralType::kInt) {
            return std::make_shared<ast::IntLit>(literal.int_val * slot.numeric_sign);
        }
        if (slot.type == SqlTemplateLiteralType::kFloat) {
            return std::make_shared<ast::FloatLit>(literal.float_val * static_cast<float>(slot.numeric_sign));
        }
        return std::make_shared<ast::StringLit>(literal.str_val);
    }
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::make_shared<ast::IntLit>(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        return std::make_shared<ast::FloatLit>(float_lit->val);
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return std::make_shared<ast::StringLit>(str_lit->val);
    }
    if (auto bool_lit = std::dynamic_pointer_cast<ast::BoolLit>(value)) {
        return std::make_shared<ast::BoolLit>(bool_lit->val);
    }
    return nullptr;
}

inline std::shared_ptr<ast::Expr> clone_expr(const std::shared_ptr<ast::Expr> &expr, CloneContext *ctx) {
    if (auto value = std::dynamic_pointer_cast<ast::Value>(expr)) {
        return clone_value(value, ctx);
    }
    if (auto col = std::dynamic_pointer_cast<ast::Col>(expr)) {
        return clone_col(col);
    }
    return nullptr;
}

inline std::shared_ptr<ast::SelectItem> clone_select_item(const std::shared_ptr<ast::SelectItem> &item,
                                                          CloneContext *ctx) {
    (void)ctx;
    if (item == nullptr) {
        return nullptr;
    }
    if (item->is_agg) {
        return std::make_shared<ast::SelectItem>(item->agg_type, clone_col(item->col), item->count_star, item->alias);
    }
    return std::make_shared<ast::SelectItem>(clone_col(item->col), item->alias);
}

inline std::shared_ptr<ast::BinaryExpr> clone_binary_expr(const std::shared_ptr<ast::BinaryExpr> &expr,
                                                          CloneContext *ctx) {
    return std::make_shared<ast::BinaryExpr>(clone_col(expr->lhs), expr->op, clone_expr(expr->rhs, ctx));
}

inline std::vector<std::shared_ptr<ast::BinaryExpr>> clone_binary_exprs(
    const std::vector<std::shared_ptr<ast::BinaryExpr>> &exprs, CloneContext *ctx) {
    std::vector<std::shared_ptr<ast::BinaryExpr>> result;
    result.reserve(exprs.size());
    for (const auto &expr : exprs) {
        result.push_back(clone_binary_expr(expr, ctx));
    }
    return result;
}

inline std::shared_ptr<ast::HavingExpr> clone_having_expr(const std::shared_ptr<ast::HavingExpr> &expr,
                                                          CloneContext *ctx) {
    return std::make_shared<ast::HavingExpr>(clone_select_item(expr->lhs, ctx), expr->op, clone_value(expr->rhs, ctx));
}

inline std::vector<std::shared_ptr<ast::HavingExpr>> clone_having_exprs(
    const std::vector<std::shared_ptr<ast::HavingExpr>> &exprs, CloneContext *ctx) {
    std::vector<std::shared_ptr<ast::HavingExpr>> result;
    result.reserve(exprs.size());
    for (const auto &expr : exprs) {
        result.push_back(clone_having_expr(expr, ctx));
    }
    return result;
}

inline std::shared_ptr<ast::OrderBy> clone_order_by(const std::shared_ptr<ast::OrderBy> &order) {
    if (order == nullptr) {
        return nullptr;
    }
    std::vector<std::shared_ptr<ast::OrderByItem>> items;
    items.reserve(order->items.size());
    for (const auto &item : order->items) {
        items.push_back(std::make_shared<ast::OrderByItem>(clone_col(item->col), item->orderby_dir));
    }
    return std::make_shared<ast::OrderBy>(std::move(items));
}

inline std::vector<std::shared_ptr<ast::TableRef>> clone_table_refs(
    const std::vector<std::shared_ptr<ast::TableRef>> &refs) {
    std::vector<std::shared_ptr<ast::TableRef>> result;
    result.reserve(refs.size());
    for (const auto &ref : refs) {
        result.push_back(std::make_shared<ast::TableRef>(ref->tab_name, ref->alias));
    }
    return result;
}

inline std::shared_ptr<ast::SelectStmt> clone_select_stmt(const std::shared_ptr<ast::SelectStmt> &select,
                                                          CloneContext *ctx) {
    std::vector<std::shared_ptr<ast::SelectItem>> select_items;
    select_items.reserve(select->select_items.size());
    for (const auto &item : select->select_items) {
        select_items.push_back(clone_select_item(item, ctx));
    }
    std::vector<std::shared_ptr<ast::Col>> group_cols;
    group_cols.reserve(select->group_cols.size());
    for (const auto &col : select->group_cols) {
        group_cols.push_back(clone_col(col));
    }
    auto conds = clone_binary_exprs(select->conds, ctx);
    auto having_conds = clone_having_exprs(select->having_conds, ctx);
    auto semi_conds = clone_binary_exprs(select->semi_conds, ctx);
    int limit = select->limit;
    if (select->has_limit && ctx != nullptr && ctx->literals != nullptr && ctx->slots != nullptr) {
        const auto &slot = (*ctx->slots)[ctx->index];
        const auto &literal = (*ctx->literals)[ctx->index++];
        limit = literal.int_val * slot.numeric_sign;
    }
    return std::make_shared<ast::SelectStmt>(
        std::move(select_items), clone_table_refs(select->table_refs), std::move(conds), std::move(group_cols),
        std::move(having_conds), clone_order_by(select->order), limit, select->is_semi_join, std::move(semi_conds));
}

inline std::shared_ptr<ast::TreeNode> clone_root(const std::shared_ptr<ast::TreeNode> &root, CloneContext *ctx) {
    if (auto insert = std::dynamic_pointer_cast<ast::InsertStmt>(root)) {
        std::vector<std::shared_ptr<ast::Value>> values;
        values.reserve(insert->vals.size());
        for (const auto &value : insert->vals) {
            values.push_back(clone_value(value, ctx));
        }
        return std::make_shared<ast::InsertStmt>(insert->tab_name, std::move(values));
    }
    if (auto update = std::dynamic_pointer_cast<ast::UpdateStmt>(root)) {
        std::vector<std::shared_ptr<ast::SetClause>> set_clauses;
        set_clauses.reserve(update->set_clauses.size());
        for (const auto &set_clause : update->set_clauses) {
            if (!set_clause->rhs_is_col) {
                set_clauses.push_back(
                    std::make_shared<ast::SetClause>(set_clause->col_name, clone_value(set_clause->val, ctx)));
            } else if (set_clause->op == ast::SET_OP_ASSIGN) {
                set_clauses.push_back(std::make_shared<ast::SetClause>(set_clause->col_name,
                                                                       set_clause->rhs_col_name));
            } else {
                set_clauses.push_back(std::make_shared<ast::SetClause>(
                    set_clause->col_name, set_clause->rhs_col_name, clone_value(set_clause->val, ctx),
                    set_clause->op));
            }
        }
        return std::make_shared<ast::UpdateStmt>(update->tab_name, std::move(set_clauses),
                                                 clone_binary_exprs(update->conds, ctx));
    }
    if (auto del = std::dynamic_pointer_cast<ast::DeleteStmt>(root)) {
        return std::make_shared<ast::DeleteStmt>(del->tab_name, clone_binary_exprs(del->conds, ctx));
    }
    if (auto select = std::dynamic_pointer_cast<ast::SelectStmt>(root)) {
        return clone_select_stmt(select, ctx);
    }
    return nullptr;
}

struct CacheEntry {
    SqlTemplateKey key;
    uint64_t schema_epoch = 0;
    std::shared_ptr<ast::TreeNode> templ;
    std::vector<LiteralSlot> slots;
};

class SqlTemplateCache {
   public:
    explicit SqlTemplateCache(size_t capacity = 256) : capacity_(capacity) {}

    std::shared_ptr<ast::TreeNode> lookup(const SqlTemplateCandidate &candidate, uint64_t epoch) {
        std::shared_ptr<ast::TreeNode> templ;
        std::vector<LiteralSlot> slots;
        {
            std::lock_guard<std::mutex> guard(latch_);
            auto it = entries_.find(candidate.key);
            if (it == entries_.end()) {
                return nullptr;
            }
            auto list_it = it->second;
            if (list_it->schema_epoch != epoch || !validate_slots(list_it->slots, candidate.literals)) {
                lru_.erase(list_it);
                entries_.erase(it);
                return nullptr;
            }
            lru_.splice(lru_.begin(), lru_, list_it);
            templ = lru_.begin()->templ;
            slots = lru_.begin()->slots;
        }
        CloneContext ctx{&candidate.literals, &slots, 0};
        auto cloned = clone_root(templ, &ctx);
        if (ctx.index != candidate.literals.size()) {
            return nullptr;
        }
        return cloned;
    }

    void store(const SqlTemplateCandidate &candidate, const std::shared_ptr<ast::TreeNode> &root, uint64_t epoch) {
        if (!root_supported(root)) {
            return;
        }
        std::vector<LiteralSlot> slots;
        collect_root_slots(root, &slots);
        if (!validate_slots(slots, candidate.literals)) {
            return;
        }
        auto templ = clone_root(root, nullptr);
        if (templ == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> guard(latch_);
        auto existing = entries_.find(candidate.key);
        if (existing != entries_.end()) {
            lru_.erase(existing->second);
            entries_.erase(existing);
        }
        lru_.push_front(CacheEntry{candidate.key, epoch, std::move(templ), std::move(slots)});
        entries_[candidate.key] = lru_.begin();
        while (entries_.size() > capacity_) {
            auto last = std::prev(lru_.end());
            entries_.erase(last->key);
            lru_.pop_back();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> guard(latch_);
        entries_.clear();
        lru_.clear();
    }

   private:
    size_t capacity_;
    std::mutex latch_;
    std::list<CacheEntry> lru_;
    std::unordered_map<SqlTemplateKey, std::list<CacheEntry>::iterator, SqlTemplateKeyHash> entries_;
};

inline SqlTemplateCache &cache_instance() {
    static SqlTemplateCache cache;
    return cache;
}

inline std::atomic<uint64_t> &schema_epoch_storage() {
    static std::atomic<uint64_t> epoch{1};
    return epoch;
}

enum class TemplateKeywordClass {
    kNone,
    kSelect,
    kInsert,
    kUpdate,
    kDelete,
    kSet,
    kTransaction,
    kUnsupported,
    kOther,
};

inline bool ascii_iequals(const char *text, size_t len, const char *word) {
    size_t word_len = std::strlen(word);
    if (len != word_len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(word[i]))) {
            return false;
        }
    }
    return true;
}

inline bool ascii_iequals_any(const char *text, size_t len, const char *const *words, size_t word_count) {
    for (size_t i = 0; i < word_count; ++i) {
        if (ascii_iequals(text, len, words[i])) {
            return true;
        }
    }
    return false;
}

inline TemplateKeywordClass keyword_class(const char *text, size_t len) {
    if (ascii_iequals(text, len, "select")) return TemplateKeywordClass::kSelect;
    if (ascii_iequals(text, len, "insert")) return TemplateKeywordClass::kInsert;
    if (ascii_iequals(text, len, "update")) return TemplateKeywordClass::kUpdate;
    if (ascii_iequals(text, len, "delete")) return TemplateKeywordClass::kDelete;
    if (ascii_iequals(text, len, "set")) return TemplateKeywordClass::kSet;
    if (ascii_iequals(text, len, "transaction")) return TemplateKeywordClass::kTransaction;

    static const char *const unsupported[] = {
        "create", "drop", "load", "desc", "show", "begin", "commit", "abort", "rollback",
        "explain", "analyze", "union", "static_checkpoint", "help", "exit", "true", "false",
    };
    if (ascii_iequals_any(text, len, unsupported, sizeof(unsupported) / sizeof(unsupported[0]))) {
        return TemplateKeywordClass::kUnsupported;
    }

    static const char *const other_keywords[] = {
        "tables", "table", "into", "values", "from", "where", "isolation", "level", "snapshot",
        "serializable", "int", "char", "float", "datetime", "index", "and", "join", "semi",
        "on", "group", "having", "limit", "as", "order", "by", "asc", "enable_nestloop",
        "enable_sortmerge", "max", "min", "count", "sum", "avg",
    };
    if (ascii_iequals_any(text, len, other_keywords, sizeof(other_keywords) / sizeof(other_keywords[0]))) {
        return TemplateKeywordClass::kOther;
    }
    return TemplateKeywordClass::kNone;
}

class TemplateKeyBuilder {
   public:
    void begin_token() {
        if (has_token_) {
            append_byte(0xff);
        }
        has_token_ = true;
    }

    void append_byte(unsigned char byte) {
        h1_ ^= byte;
        h1_ *= 1099511628211ULL;
        h2_ ^= static_cast<uint64_t>(byte) + 0x9e3779b97f4a7c15ULL + (h2_ << 6) + (h2_ >> 2);
        ++normalized_len_;
    }

    void append_token(const char *text, size_t len, bool lowercase) {
        begin_token();
        for (size_t i = 0; i < len; ++i) {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            append_byte(lowercase ? static_cast<unsigned char>(std::tolower(ch)) : ch);
        }
    }

    void append_token(const char *text) {
        append_token(text, std::strlen(text), false);
    }

    SqlTemplateKey finish(size_t literal_count) const {
        SqlTemplateKey key;
        key.h1 = h1_;
        key.h2 = h2_;
        key.normalized_len = normalized_len_;
        key.literal_count = static_cast<uint16_t>(literal_count);
        return key;
    }

   private:
    uint64_t h1_ = 1469598103934665603ULL;
    uint64_t h2_ = 0x9e3779b97f4a7c15ULL;
    uint32_t normalized_len_ = 0;
    bool has_token_ = false;
};

inline bool read_string_literal_raw(const char *sql, size_t *pos, std::string *value) {
    size_t i = *pos + 1;
    while (sql[i] != '\0' && sql[i] != '\'') {
        ++i;
    }
    if (sql[i] == '\0') {
        return false;
    }
    value->assign(sql + *pos + 1, i - *pos - 1);
    *pos = i + 1;
    return true;
}

inline bool read_numeric_literal_raw(const char *sql, size_t *pos, SqlTemplateLiteral *literal,
                                     const char **placeholder) {
    size_t start = *pos;
    size_t i = start;
    int int_value = 0;
    while (std::isdigit(static_cast<unsigned char>(sql[i])) != 0) {
        int_value = int_value * 10 + (sql[i] - '0');
        ++i;
    }
    bool is_float = false;
    if (sql[i] == '.') {
        is_float = true;
        ++i;
        while (std::isdigit(static_cast<unsigned char>(sql[i])) != 0) {
            ++i;
        }
    }
    if (i == start) {
        return false;
    }
    if (is_float) {
        literal->type = SqlTemplateLiteralType::kFloat;
        literal->float_val = std::strtof(sql + start, nullptr);
        *placeholder = "?f";
    } else {
        literal->type = SqlTemplateLiteralType::kInt;
        literal->int_val = int_value;
        *placeholder = "?i";
    }
    *pos = i;
    return true;
}

}  // namespace sql_template_detail

inline uint64_t sql_template_schema_epoch() {
    return sql_template_detail::schema_epoch_storage().load(std::memory_order_acquire);
}

inline void bump_sql_template_schema_epoch() {
    sql_template_detail::schema_epoch_storage().fetch_add(1, std::memory_order_acq_rel);
    sql_template_detail::cache_instance().clear();
}

inline std::optional<SqlTemplateCandidate> make_sql_template_candidate(const char *raw_sql) {
    if (raw_sql == nullptr) {
        return std::nullopt;
    }
    sql_template_detail::TemplateKeyBuilder key_builder;
    SqlTemplateCandidate candidate;
    bool saw_token = false;
    bool first_token_cacheable = false;
    bool unsupported = false;
    bool previous_set = false;
    size_t i = 0;
    while (raw_sql[i] != '\0') {
        unsigned char ch = static_cast<unsigned char>(raw_sql[i]);
        if (std::isspace(ch) != 0) {
            ++i;
            continue;
        }
        if ((raw_sql[i] == '-' && raw_sql[i + 1] == '-') || (raw_sql[i] == '/' && raw_sql[i + 1] == '*')) {
            return std::nullopt;
        }
        if (std::isalpha(ch) != 0) {
            size_t start = i++;
            while (raw_sql[i] != '\0') {
                unsigned char next = static_cast<unsigned char>(raw_sql[i]);
                if (std::isalnum(next) == 0 && raw_sql[i] != '_') {
                    break;
                }
                ++i;
            }
            size_t len = i - start;
            auto keyword_class = sql_template_detail::keyword_class(raw_sql + start, len);
            bool is_keyword = keyword_class != sql_template_detail::TemplateKeywordClass::kNone;
            if (!saw_token) {
                first_token_cacheable = keyword_class == sql_template_detail::TemplateKeywordClass::kSelect ||
                                        keyword_class == sql_template_detail::TemplateKeywordClass::kInsert ||
                                        keyword_class == sql_template_detail::TemplateKeywordClass::kUpdate ||
                                        keyword_class == sql_template_detail::TemplateKeywordClass::kDelete;
            }
            if (keyword_class == sql_template_detail::TemplateKeywordClass::kUnsupported ||
                (previous_set && keyword_class == sql_template_detail::TemplateKeywordClass::kTransaction)) {
                unsupported = true;
            }
            previous_set = keyword_class == sql_template_detail::TemplateKeywordClass::kSet;
            key_builder.append_token(raw_sql + start, len, is_keyword);
            saw_token = true;
            continue;
        }
        if (std::isdigit(ch) != 0) {
            SqlTemplateLiteral literal;
            const char *placeholder = nullptr;
            if (!sql_template_detail::read_numeric_literal_raw(raw_sql, &i, &literal, &placeholder)) {
                return std::nullopt;
            }
            if (!candidate.literals.push_back(std::move(literal))) {
                return std::nullopt;
            }
            key_builder.append_token(placeholder);
            previous_set = false;
            saw_token = true;
            continue;
        }
        if (raw_sql[i] == '\'') {
            SqlTemplateLiteral literal;
            literal.type = SqlTemplateLiteralType::kString;
            if (!sql_template_detail::read_string_literal_raw(raw_sql, &i, &literal.str_val)) {
                return std::nullopt;
            }
            if (!candidate.literals.push_back(std::move(literal))) {
                return std::nullopt;
            }
            key_builder.append_token("?s");
            previous_set = false;
            saw_token = true;
            continue;
        }
        if (raw_sql[i + 1] != '\0') {
            if ((raw_sql[i] == '>' && raw_sql[i + 1] == '=') ||
                (raw_sql[i] == '<' && (raw_sql[i + 1] == '=' || raw_sql[i + 1] == '>'))) {
                key_builder.append_token(raw_sql + i, 2, false);
                i += 2;
                previous_set = false;
                saw_token = true;
                continue;
            }
        }
        static constexpr const char *single_ops = ";(),*/=<>.+-";
        if (std::strchr(single_ops, raw_sql[i]) != nullptr) {
            key_builder.append_token(raw_sql + i, 1, false);
            ++i;
            previous_set = false;
            saw_token = true;
            continue;
        }
        return std::nullopt;
    }
    if (!saw_token || !first_token_cacheable || unsupported ||
        candidate.literals.size() > std::numeric_limits<uint16_t>::max()) {
        return std::nullopt;
    }
    candidate.key = key_builder.finish(candidate.literals.size());
    return candidate;
}

inline std::shared_ptr<ast::TreeNode> lookup_sql_template(const SqlTemplateCandidate &candidate) {
    return sql_template_detail::cache_instance().lookup(candidate, sql_template_schema_epoch());
}

inline void store_sql_template(const SqlTemplateCandidate &candidate, const std::shared_ptr<ast::TreeNode> &root) {
    sql_template_detail::cache_instance().store(candidate, root, sql_template_schema_epoch());
}

inline bool is_sql_template_cacheable_ast(const std::shared_ptr<ast::TreeNode> &root) {
    return sql_template_detail::root_supported(root);
}

inline std::shared_ptr<ast::TreeNode> clone_sql_template_ast(const std::shared_ptr<ast::TreeNode> &root) {
    if (!sql_template_detail::root_supported(root)) {
        return nullptr;
    }
    return sql_template_detail::clone_root(root, nullptr);
}

}  // namespace rmdb
