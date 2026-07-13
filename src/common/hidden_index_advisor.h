#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyze/analyze.h"
#include "common/schema_change.h"
#include "index/ix_defs.h"
#include "system/sm_manager.h"
#include "transaction/transaction_manager.h"

namespace rmdb {

struct HiddenIndexCandidate {
    std::string tab_name;
    std::vector<std::string> col_names;
    int threshold = 16;
    int benefit = 1;
    int priority = 0;
    int key_len = 0;
};

namespace hidden_index_advisor_detail {

static constexpr int kMaxHiddenIndexes = 8;
static constexpr int kMaxHiddenIndexesPerTable = 2;

struct CandidateState {
    HiddenIndexCandidate candidate;
    int score = 0;
    int select_hits = 0;
    bool building = false;
    bool built = false;
};

inline std::mutex &advisor_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::string, CandidateState> &candidate_states() {
    static std::unordered_map<std::string, CandidateState> states;
    return states;
}

inline std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

inline std::string trim_copy(std::string text) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), is_space));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), is_space).base(), text.end());
    return text;
}

inline bool env_enabled() {
    const char *env = std::getenv("RMDB_ADVISOR_ENABLED");
    if (env == nullptr || env[0] == '\0') {
        return true;
    }
    std::string value = lower_copy(trim_copy(env));
    return value != "0" && value != "false" && value != "off" && value != "no";
}

inline int observation_seconds() {
    static const int configured = [] {
        const char *env = std::getenv("RMDB_ADVISOR_OBSERVE_SECONDS");
        if (env == nullptr || env[0] == '\0') {
            return 60;
        }
        try {
            int value = std::stoi(env);
            return value < 0 ? 60 : std::min(value, 86400);
        } catch (...) {
            return 60;
        }
    }();
    return configured;
}

inline bool observation_window_open() {
    const int seconds = observation_seconds();
    if (seconds == 0) {
        return true;
    }
    static const auto first_observation = std::chrono::steady_clock::now();
    return std::chrono::steady_clock::now() - first_observation < std::chrono::seconds(seconds);
}

inline bool add_unique(std::vector<std::string> *cols, const std::string &col_name) {
    if (col_name.empty() || std::find(cols->begin(), cols->end(), col_name) != cols->end()) {
        return false;
    }
    cols->push_back(col_name);
    return true;
}

inline bool ends_with_id(const std::string &name) {
    return name == "id" || (name.size() >= 3 && name.compare(name.size() - 3, 3, "_id") == 0);
}

inline bool starts_with_cols(const IndexMeta &index, const std::vector<std::string> &cols) {
    if (index.cols.size() < cols.size()) {
        return false;
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        if (index.cols[i].name != cols[i]) {
            return false;
        }
    }
    return true;
}

inline bool has_equivalent_or_better_index(const TabMeta &tab, const std::vector<std::string> &cols) {
    for (const auto &index : tab.indexes) {
        if (starts_with_cols(index, cols)) {
            return true;
        }
    }
    return false;
}

inline int hidden_index_count(const TabMeta &tab) {
    int count = 0;
    for (const auto &index : tab.indexes) {
        if (index.hidden) {
            count++;
        }
    }
    return count;
}

inline int hidden_index_count(const SmManager *sm_manager) {
    return sm_manager->hidden_index_count();
}

inline int target_hidden_index_count() {
    const char *env = std::getenv("RMDB_ADVISOR_TARGET_INDEXES");
    if (env == nullptr || env[0] == '\0') {
        return kMaxHiddenIndexes;
    }
    try {
        int value = std::stoi(env);
        if (value <= 0) {
            return kMaxHiddenIndexes;
        }
        return std::min(value, kMaxHiddenIndexes);
    } catch (...) {
        return kMaxHiddenIndexes;
    }
}

inline int logical_key_len(const TabMeta &tab, const std::vector<std::string> &cols) {
    int len = 0;
    for (const auto &col_name : cols) {
        auto it = std::find_if(tab.cols.begin(), tab.cols.end(), [&](const ColMeta &col) {
            return col.name == col_name;
        });
        if (it == tab.cols.end()) {
            return -1;
        }
        len += it->len;
    }
    return len;
}

inline bool candidate_key_valid(const TabMeta &tab, const std::vector<std::string> &cols) {
    if (cols.empty()) {
        return false;
    }
    int len = logical_key_len(tab, cols);
    return len > 0 && len <= IX_MAX_COL_LEN;
}

inline bool candidate_covers_projection(const std::shared_ptr<Query> &query, const std::string &visible,
                                        const std::vector<std::string> &cols) {
    if (query == nullptr || query->cols.empty()) {
        return false;
    }
    bool saw_visible_col = false;
    for (const auto &col : query->cols) {
        if (!col.tab_name.empty() && col.tab_name != visible) {
            continue;
        }
        saw_visible_col = true;
        if (col.col_name.empty() || std::find(cols.begin(), cols.end(), col.col_name) == cols.end()) {
            return false;
        }
    }
    return saw_visible_col;
}

inline std::string state_key(const HiddenIndexCandidate &candidate) {
    std::string key = candidate.tab_name;
    key.push_back('\x1f');
    for (const auto &col : candidate.col_names) {
        key += col;
        key.push_back('\x1e');
    }
    return key;
}

inline std::string candidate_label(const HiddenIndexCandidate &candidate) {
    std::string out = candidate.tab_name + "(";
    for (size_t i = 0; i < candidate.col_names.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += candidate.col_names[i];
    }
    out += ")";
    return out;
}

inline int candidate_shape_cost(const HiddenIndexCandidate &candidate) {
    return static_cast<int>(candidate.col_names.size() * 10) + candidate.threshold;
}

inline bool candidate_state_better(const CandidateState &lhs, const std::string &lhs_key,
                                   const CandidateState &rhs, const std::string &rhs_key) {
    if (lhs.candidate.priority != rhs.candidate.priority) {
        return lhs.candidate.priority > rhs.candidate.priority;
    }
    if (lhs.candidate.threshold != rhs.candidate.threshold) {
        return lhs.candidate.threshold < rhs.candidate.threshold;
    }
    if (lhs.candidate.key_len > 0 && rhs.candidate.key_len > 0 &&
        lhs.candidate.key_len != rhs.candidate.key_len) {
        return lhs.candidate.key_len < rhs.candidate.key_len;
    }
    int lhs_shape_cost = candidate_shape_cost(lhs.candidate);
    int rhs_shape_cost = candidate_shape_cost(rhs.candidate);
    if (lhs_shape_cost != rhs_shape_cost) {
        return lhs_shape_cost < rhs_shape_cost;
    }
    if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
    }
    if (lhs.candidate.benefit != rhs.candidate.benefit) {
        return lhs.candidate.benefit > rhs.candidate.benefit;
    }
    if (lhs.select_hits != rhs.select_hits) {
        return lhs.select_hits > rhs.select_hits;
    }
    return lhs_key < rhs_key;
}

inline bool is_simple_select_shape(const std::shared_ptr<Query> &query) {
    return query != nullptr && query->kind == StmtKind::Select && query->union_queries.empty() &&
           query->having_conds.empty() && !query->is_semi_join && query->semi_conds.empty() &&
           !query->has_group && query->tables.size() >= 1;
}

inline bool is_supported_isolation(Context *context) {
    return context != nullptr && context->txn_ != nullptr &&
           (context->txn_->get_isolation_level() == IsolationLevel::READ_COMMITTED ||
            context->txn_->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION);
}

inline std::string base_table_for(const std::shared_ptr<Query> &query, const std::string &visible) {
    auto it = query->alias_to_table.find(visible);
    return it == query->alias_to_table.end() ? visible : it->second;
}

inline std::string min_aggregate_col_for(const std::shared_ptr<Query> &query, const std::string &visible) {
    if (!query->has_aggregate || query->select_items.size() != 1) {
        return "";
    }
    const auto &item = query->select_items[0];
    if (item == nullptr || !item->is_agg || item->count_star || item->agg_type != ast::AGG_MIN ||
        item->col == nullptr || item->col->tab_name != visible) {
        return "";
    }
    return item->col->col_name;
}

inline std::string tie_break_col_for(const TabMeta &tab, const std::vector<std::string> &cols,
                                     const std::vector<TabCol> &output_cols, const std::string &visible) {
    for (const auto &col : output_cols) {
        if (col.tab_name == visible && ends_with_id(col.col_name) &&
            std::find(cols.begin(), cols.end(), col.col_name) == cols.end()) {
            return col.col_name;
        }
    }
    for (const auto &col : tab.cols) {
        if (ends_with_id(col.name) && std::find(cols.begin(), cols.end(), col.name) == cols.end()) {
            return col.name;
        }
    }
    for (const auto &col : output_cols) {
        if (col.tab_name == visible && std::find(cols.begin(), cols.end(), col.col_name) == cols.end()) {
            return col.col_name;
        }
    }
    return "";
}

inline void collect_table_candidate(SmManager *sm_manager, const std::shared_ptr<Query> &query,
                                    const std::string &visible, std::vector<HiddenIndexCandidate> *out) {
    const std::string base = base_table_for(query, visible);
    if (!sm_manager->db_.is_table(base)) {
        return;
    }
    TabMeta &tab = sm_manager->db_.get_table(base);

    std::vector<std::string> cols;
    std::vector<std::string> range_cols;
    for (const auto &cond : query->conds) {
        if (!cond.is_rhs_val || cond.lhs_col.tab_name != visible || cond.op == OP_NE) {
            continue;
        }
        if (cond.op == OP_EQ) {
            add_unique(&cols, cond.lhs_col.col_name);
        } else {
            add_unique(&range_cols, cond.lhs_col.col_name);
        }
    }
    if (cols.empty()) {
        return;
    }

    bool added_order = false;
    if (!query->order_cols.empty()) {
        for (const auto &order_col : query->order_cols) {
            if (order_col.first.tab_name == visible) {
                added_order = add_unique(&cols, order_col.first.col_name) || added_order;
            }
        }
    }

    bool added_min = false;
    if (!added_order && query->order_cols.empty()) {
        std::string min_col = min_aggregate_col_for(query, visible);
        if (!min_col.empty()) {
            added_min = add_unique(&cols, min_col);
        }
    }

    bool added_range = false;
    if (!added_order && !added_min && query->order_cols.empty() && range_cols.size() == 1) {
        added_range = add_unique(&cols, range_cols.front());
    }

    if (!added_order && !added_min && !added_range) {
        return;
    }

    std::string tie_col = tie_break_col_for(tab, cols, query->cols, visible);
    if (!tie_col.empty()) {
        add_unique(&cols, tie_col);
    }

    int key_len = logical_key_len(tab, cols);
    if (key_len <= 0 || key_len > IX_MAX_COL_LEN || has_equivalent_or_better_index(tab, cols)) {
        return;
    }
    int threshold = added_order || added_min ? 8 : 16;
    int benefit = 1;
    int priority = 0;
    if (added_order && query->has_limit) {
        benefit = 6;
        priority = 300;
    } else if (added_order || added_min) {
        benefit = 4;
        priority = added_min ? 400 : 200;
    } else if (added_range) {
        benefit = 2;
        priority = 100;
    }
    if (candidate_covers_projection(query, visible, cols)) {
        benefit += 2;
    }
    out->push_back({base, std::move(cols), threshold, benefit, priority, key_len});
}

inline void collect_join_candidate(SmManager *sm_manager, const std::shared_ptr<Query> &query,
                                   const std::string &visible, std::vector<HiddenIndexCandidate> *out) {
    const std::string base = base_table_for(query, visible);
    if (!sm_manager->db_.is_table(base)) {
        return;
    }
    TabMeta &tab = sm_manager->db_.get_table(base);
    std::vector<std::string> cols;
    for (const auto &cond : query->conds) {
        if (cond.is_rhs_val || cond.op != OP_EQ) {
            continue;
        }
        if (cond.lhs_col.tab_name == visible && cond.rhs_col.tab_name != visible) {
            add_unique(&cols, cond.lhs_col.col_name);
        } else if (cond.rhs_col.tab_name == visible && cond.lhs_col.tab_name != visible) {
            add_unique(&cols, cond.rhs_col.col_name);
        }
    }
    if (cols.empty() || !candidate_key_valid(tab, cols) || has_equivalent_or_better_index(tab, cols)) {
        return;
    }
    out->push_back({base, std::move(cols), 32, 1});
}

inline bool can_publish_candidate(SmManager *sm_manager, const HiddenIndexCandidate &candidate) {
    if (!sm_manager->db_.is_table(candidate.tab_name)) {
        return false;
    }
    TabMeta &tab = sm_manager->db_.get_table(candidate.tab_name);
    if (hidden_index_count(tab) >= kMaxHiddenIndexesPerTable || hidden_index_count(sm_manager) >= kMaxHiddenIndexes) {
        return false;
    }
    return candidate_key_valid(tab, candidate.col_names) &&
           !has_equivalent_or_better_index(tab, candidate.col_names);
}

inline bool has_enough_net_benefit(const CandidateState &state) {
    return state.score >= state.candidate.threshold;
}

}  // namespace hidden_index_advisor_detail

inline bool hidden_index_advisor_enabled() {
    return hidden_index_advisor_detail::env_enabled();
}

inline bool hidden_index_advisor_should_observe_sql(SmManager *sm_manager, const char *raw_sql,
                                                    IsolationLevel isolation_level) {
    if (!hidden_index_advisor_enabled() ||
        (isolation_level != IsolationLevel::READ_COMMITTED &&
         isolation_level != IsolationLevel::SNAPSHOT_ISOLATION) ||
        raw_sql == nullptr) {
        return false;
    }
    std::string sql = hidden_index_advisor_detail::lower_copy(hidden_index_advisor_detail::trim_copy(raw_sql));
    if (!sql.empty() && sql.back() == ';') {
        sql.pop_back();
        sql = hidden_index_advisor_detail::trim_copy(sql);
    }
    if (!(sql == "select" || sql.rfind("select ", 0) == 0)) {
        return false;
    }
    if (sm_manager != nullptr) {
        if (!hidden_index_advisor_detail::observation_window_open()) {
            return false;
        }
        return hidden_index_advisor_detail::hidden_index_count(sm_manager) <
               hidden_index_advisor_detail::target_hidden_index_count();
    }
    std::lock_guard<std::mutex> guard(hidden_index_advisor_detail::advisor_mutex());
    for (const auto &entry : hidden_index_advisor_detail::candidate_states()) {
        const auto &state = entry.second;
        if (!state.built || state.building) {
            return true;
        }
    }
    return hidden_index_advisor_detail::candidate_states().empty();
}

inline bool hidden_index_advisor_should_observe_sql(const char *raw_sql, IsolationLevel isolation_level) {
    return hidden_index_advisor_should_observe_sql(nullptr, raw_sql, isolation_level);
}

inline std::vector<HiddenIndexCandidate> hidden_index_advisor_collect(SmManager *sm_manager,
                                                                      const std::shared_ptr<Query> &query,
                                                                      Context *context) {
    std::vector<HiddenIndexCandidate> candidates;
    if (!hidden_index_advisor_enabled() || !hidden_index_advisor_detail::is_supported_isolation(context) ||
        !hidden_index_advisor_detail::is_simple_select_shape(query)) {
        return candidates;
    }
    for (const auto &visible : query->tables) {
        hidden_index_advisor_detail::collect_table_candidate(sm_manager, query, visible, &candidates);
        if (query->tables.size() > 1) {
            hidden_index_advisor_detail::collect_join_candidate(sm_manager, query, visible, &candidates);
        }
    }
    return candidates;
}

inline void hidden_index_advisor_record(const std::vector<HiddenIndexCandidate> &candidates) {
    if (!hidden_index_advisor_enabled() || candidates.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(hidden_index_advisor_detail::advisor_mutex());
    auto &states = hidden_index_advisor_detail::candidate_states();
    for (const auto &candidate : candidates) {
        std::string key = hidden_index_advisor_detail::state_key(candidate);
        auto &state = states[key];
        if (state.candidate.tab_name.empty()) {
            state.candidate = candidate;
        } else if (candidate.threshold < state.candidate.threshold) {
            state.candidate.threshold = candidate.threshold;
            state.candidate.benefit = std::max(state.candidate.benefit, candidate.benefit);
        } else {
            state.candidate.benefit = std::max(state.candidate.benefit, candidate.benefit);
        }
        if (!state.built) {
            state.score += std::max(1, candidate.benefit);
            state.select_hits++;
        }
    }
}

inline bool hidden_index_advisor_try_build_one(SmManager *sm_manager, TransactionManager *txn_manager,
                                               Context *context) {
    if (!hidden_index_advisor_enabled() || sm_manager == nullptr || txn_manager == nullptr) {
        return false;
    }

    std::string selected_key;
    HiddenIndexCandidate selected;
    {
        std::lock_guard<std::mutex> guard(hidden_index_advisor_detail::advisor_mutex());
        auto &states = hidden_index_advisor_detail::candidate_states();
        hidden_index_advisor_detail::CandidateState *selected_state = nullptr;
        for (auto &entry : states) {
            auto &state = entry.second;
            if (state.built || state.building || !hidden_index_advisor_detail::has_enough_net_benefit(state)) {
                continue;
            }
            if (selected_state == nullptr ||
                hidden_index_advisor_detail::candidate_state_better(state, entry.first, *selected_state, selected_key)) {
                selected_key = entry.first;
                selected_state = &state;
            }
        }
        if (selected_state != nullptr) {
            selected = selected_state->candidate;
            selected_state->building = true;
        }
    }
    if (selected_key.empty()) {
        return false;
    }

    // A continuously loaded explicit-transaction workload may never have an
    // observable zero-active instant.  Once a candidate is mature, briefly
    // close admission so existing transactions can drain.  The timeout keeps
    // advisor work from recreating an unbounded workload pause.
    auto drain_guard = txn_manager->BlockNewTransactionsAndWaitFor(std::chrono::seconds(2));
    if (!drain_guard) {
        std::lock_guard<std::mutex> guard(hidden_index_advisor_detail::advisor_mutex());
        auto it = hidden_index_advisor_detail::candidate_states().find(selected_key);
        if (it != hidden_index_advisor_detail::candidate_states().end()) {
            it->second.building = false;
        }
        return false;
    }

    bool built = false;
    bool retired = false;
    try {
        auto checkpoint_guard = txn_manager->EnterCheckpointExecution();
        if (hidden_index_advisor_detail::can_publish_candidate(sm_manager, selected)) {
            built = sm_manager->create_internal_non_unique_index(selected.tab_name, selected.col_names, context);
        } else {
            retired = true;
        }
    } catch (const RMDBError &err) {
        std::cerr << "hidden index advisor failed for "
                  << hidden_index_advisor_detail::candidate_label(selected) << ": " << err.what() << "\n";
    } catch (const std::exception &err) {
        std::cerr << "hidden index advisor failed for "
                  << hidden_index_advisor_detail::candidate_label(selected) << ": " << err.what() << "\n";
    }

    {
        std::lock_guard<std::mutex> guard(hidden_index_advisor_detail::advisor_mutex());
        auto it = hidden_index_advisor_detail::candidate_states().find(selected_key);
        if (it != hidden_index_advisor_detail::candidate_states().end()) {
            it->second.building = false;
            it->second.built = built || retired || it->second.built;
            if (!built && !retired) {
                it->second.score = std::max(0, it->second.score - 1);
            }
        }
    }
    if (built) {
        invalidate_sql_template_caches();
        std::cout << "hidden index advisor created "
                  << hidden_index_advisor_detail::candidate_label(selected) << "\n";
    }
    return built;
}

}  // namespace rmdb
