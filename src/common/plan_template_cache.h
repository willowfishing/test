#pragma once

#include <algorithm>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/query_template_cache.h"
#include "optimizer/plan.h"

namespace rmdb {

namespace plan_template_detail {

enum class PlanLiteralTarget {
    kDmlValue,
    kDmlSetValue,
    kDmlCondValue,
    kScanCondValue,
    kJoinCondValue,
    kSemiCondValue,
    kHavingValue,
    kLimitValue,
    kSortLimitValue,
};

struct PlanLiteralSlot {
    PlanLiteralTarget target = PlanLiteralTarget::kDmlValue;
    uint32_t plan_node = 0;
    uint32_t index = 0;
    uint32_t source_index = 0;
    query_template_detail::QueryLiteralSlot query_slot;
};

struct PlanTemplate {
    std::shared_ptr<Plan> skeleton;
    std::vector<PlanLiteralSlot> slots;
    std::shared_ptr<RuntimeFeedbackStore> feedback;
    size_t literal_count = 0;
};

struct PlanTemplateKey {
    SqlTemplateKey sql;
    IsolationLevel isolation_level = IsolationLevel::READ_COMMITTED;

    bool operator==(const PlanTemplateKey &other) const {
        return sql == other.sql && isolation_level == other.isolation_level;
    }
};

struct PlanTemplateKeyHash {
    size_t operator()(const PlanTemplateKey &key) const {
        size_t hash = SqlTemplateKeyHash{}(key.sql);
        hash ^= static_cast<size_t>(key.isolation_level) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct CacheEntry {
    PlanTemplateKey key;
    uint64_t schema_epoch = 0;
    std::shared_ptr<const PlanTemplate> templ;
};

inline bool cacheable_dml_tag(PlanTag tag) {
    return tag == T_select || tag == T_Insert || tag == T_Update || tag == T_Delete;
}

inline query_template_detail::QueryLiteralTarget query_target_for_plan_target(PlanLiteralTarget target) {
    switch (target) {
        case PlanLiteralTarget::kDmlValue:
            return query_template_detail::QueryLiteralTarget::kQueryValue;
        case PlanLiteralTarget::kDmlSetValue:
            return query_template_detail::QueryLiteralTarget::kUpdateSetValue;
        case PlanLiteralTarget::kDmlCondValue:
        case PlanLiteralTarget::kScanCondValue:
        case PlanLiteralTarget::kJoinCondValue:
            return query_template_detail::QueryLiteralTarget::kWhereCondValue;
        case PlanLiteralTarget::kSemiCondValue:
            return query_template_detail::QueryLiteralTarget::kSemiCondValue;
        case PlanLiteralTarget::kHavingValue:
            return query_template_detail::QueryLiteralTarget::kHavingValue;
        case PlanLiteralTarget::kLimitValue:
        case PlanLiteralTarget::kSortLimitValue:
            return query_template_detail::QueryLiteralTarget::kLimitValue;
    }
    return query_template_detail::QueryLiteralTarget::kQueryValue;
}

inline bool same_tab_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

inline bool same_literal_value(const Value &lhs, const Value &rhs) {
    if (lhs.type != rhs.type) {
        return false;
    }
    if (lhs.type == TYPE_INT) {
        return lhs.int_val == rhs.int_val;
    }
    if (lhs.type == TYPE_FLOAT) {
        return lhs.float_val == rhs.float_val;
    }
    return lhs.str_val == rhs.str_val;
}

inline bool condition_matches_literal(const Condition &plan_cond, const Condition &query_cond) {
    return plan_cond.is_rhs_val && query_cond.is_rhs_val && same_tab_col(plan_cond.lhs_col, query_cond.lhs_col) &&
           plan_cond.op == query_cond.op && same_literal_value(plan_cond.rhs_val, query_cond.rhs_val);
}

inline bool set_clause_matches_literal(const SetClause &plan_clause, const SetClause &query_clause) {
    return same_tab_col(plan_clause.lhs, query_clause.lhs) && plan_clause.op == query_clause.op &&
           plan_clause.rhs_is_col == query_clause.rhs_is_col && same_tab_col(plan_clause.rhs_col, query_clause.rhs_col) &&
           plan_clause.rhs_has_val && query_clause.rhs_has_val && same_literal_value(plan_clause.rhs, query_clause.rhs);
}

inline bool ast_value_matches_literal(const std::shared_ptr<ast::Value> &value,
                                      const query_template_detail::QueryLiteralSlot &slot,
                                      const SqlTemplateLiteral &literal) {
    if (value == nullptr || slot.parsed_slot.type != literal.type) {
        return false;
    }
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kInt &&
               int_lit->val == literal.int_val * slot.parsed_slot.numeric_sign;
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kFloat &&
               float_lit->val == literal.float_val * static_cast<float>(slot.parsed_slot.numeric_sign);
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return literal.type == SqlTemplateLiteralType::kString && str_lit->val == literal.str_val;
    }
    return false;
}

class PlanCloneBuilder {
   public:
    uint32_t next_node() { return next_node_++; }
    uint32_t node_count() const { return next_node_; }

   private:
    uint32_t next_node_ = 0;
};

class PlanMaterializer {
   public:
    PlanMaterializer() = default;
    explicit PlanMaterializer(size_t expected_nodes) { nodes_.reserve(expected_nodes); }

    void set_root(std::shared_ptr<Plan> root) { root_ = std::move(root); }

    void register_node(uint32_t id, const std::shared_ptr<Plan> &plan) {
        if (nodes_.size() <= id) {
            nodes_.resize(static_cast<size_t>(id) + 1);
        }
        nodes_[id] = plan;
    }

    std::shared_ptr<Plan> root() const { return root_; }

    std::shared_ptr<Plan> get(uint32_t id) const {
        return id < nodes_.size() ? nodes_[id] : nullptr;
    }

   private:
    std::shared_ptr<Plan> root_;
    std::vector<std::shared_ptr<Plan>> nodes_;
};

inline std::shared_ptr<Plan> clone_plan_skeleton(const std::shared_ptr<Plan> &src, PlanCloneBuilder *builder,
                                                 PlanMaterializer *materializer = nullptr);

inline void copy_runtime_cache(const std::shared_ptr<Plan> &src, const std::shared_ptr<Plan> &dst) {
    if (src != nullptr && dst != nullptr) {
        dst->runtime_cache_ = src->runtime_cache_;
        dst->runtime_feedback_ = src->runtime_feedback_;
    }
}

inline void assign_template_node(const std::shared_ptr<Plan> &plan, uint32_t node_id,
                                 PlanMaterializer *materializer) {
    plan->template_node_id_ = node_id;
    if (materializer != nullptr) {
        materializer->register_node(node_id, plan);
    }
}

inline std::vector<Value> clone_values(const std::vector<Value> &src) {
    std::vector<Value> dst;
    dst.reserve(src.size());
    for (const auto &value : src) {
        dst.push_back(query_template_detail::clone_value(value));
    }
    return dst;
}

inline std::vector<Condition> clone_conditions(const std::vector<Condition> &src) {
    std::vector<Condition> dst;
    dst.reserve(src.size());
    for (const auto &cond : src) {
        dst.push_back(query_template_detail::clone_condition(cond));
    }
    return dst;
}

inline std::vector<SetClause> clone_set_clauses(const std::vector<SetClause> &src) {
    std::vector<SetClause> dst;
    dst.reserve(src.size());
    for (const auto &clause : src) {
        dst.push_back(query_template_detail::clone_set_clause(clause));
    }
    return dst;
}

inline std::vector<std::shared_ptr<ast::SelectItem>> clone_select_items(
    const std::vector<std::shared_ptr<ast::SelectItem>> &src) {
    std::vector<std::shared_ptr<ast::SelectItem>> dst;
    dst.reserve(src.size());
    for (const auto &item : src) {
        dst.push_back(sql_template_detail::clone_select_item(item, nullptr));
    }
    return dst;
}

inline std::shared_ptr<Plan> clone_scan_plan(const std::shared_ptr<ScanPlan> &src, PlanCloneBuilder *builder,
                                             PlanMaterializer *materializer) {
    auto dst = std::make_shared<ScanPlan>(*src);
    copy_runtime_cache(src, dst);
    dst->conds_ = clone_conditions(src->conds_);
    dst->fed_conds_ = clone_conditions(src->fed_conds_);
    uint32_t id = builder->next_node();
    assign_template_node(dst, id, materializer);
    return dst;
}

inline std::shared_ptr<Plan> clone_plan_skeleton(const std::shared_ptr<Plan> &src, PlanCloneBuilder *builder,
                                                 PlanMaterializer *materializer) {
    if (src == nullptr) {
        return nullptr;
    }
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(src)) {
        if (!cacheable_dml_tag(dml->tag)) {
            return nullptr;
        }
        auto subplan = clone_plan_skeleton(dml->subplan_, builder, materializer);
        auto dst = std::make_shared<DMLPlan>(dml->tag, subplan, dml->tab_name_, clone_values(dml->values_),
                                             clone_conditions(dml->conds_), clone_set_clauses(dml->set_clauses_));
        copy_runtime_cache(src, dst);
        dst->file_name_ = dml->file_name_;
        dst->output_cols_ = dml->output_cols_;
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(src)) {
        if (scan->tag != T_SeqScan && scan->tag != T_IndexScan) {
            return nullptr;
        }
        return clone_scan_plan(scan, builder, materializer);
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(src)) {
        if (join->tag != T_NestLoop && join->tag != T_IndexNestLoop && join->tag != T_HashJoin &&
            join->tag != T_SortMerge) {
            return nullptr;
        }
        auto left = clone_plan_skeleton(join->left_, builder, materializer);
        auto right = clone_plan_skeleton(join->right_, builder, materializer);
        if (left == nullptr || right == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<JoinPlan>(join->tag, left, right, clone_conditions(join->conds_),
                                              join->index_col_names_);
        copy_runtime_cache(src, dst);
        dst->type = join->type;
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(src)) {
        auto subplan = clone_plan_skeleton(projection->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<ProjectionPlan>(projection->tag, subplan, projection->sel_cols_);
        copy_runtime_cache(src, dst);
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(src)) {
        auto subplan = clone_plan_skeleton(sort->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<SortPlan>(sort->tag, subplan, sort->order_cols_);
        copy_runtime_cache(src, dst);
        dst->sel_col_ = sort->sel_col_;
        dst->is_desc_ = sort->is_desc_;
        dst->limit_ = sort->limit_;
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(src)) {
        auto subplan = clone_plan_skeleton(aggregate->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<AggregatePlan>(
            subplan, clone_select_items(aggregate->select_items_), aggregate->group_cols_,
            sql_template_detail::clone_having_exprs(aggregate->having_conds_, nullptr), aggregate->output_cols_);
        copy_runtime_cache(src, dst);
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(src)) {
        auto dst = std::make_shared<CountIndexAggregatePlan>(
            count->tab_name_, count->visible_name_, clone_conditions(count->conds_), count->index_col_names_,
            count->count_star_, count->count_col_, count->output_cols_);
        copy_runtime_cache(src, dst);
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(src)) {
        auto subplan = clone_plan_skeleton(limit->subplan_, builder, materializer);
        if (subplan == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<LimitPlan>(subplan, limit->limit_);
        copy_runtime_cache(src, dst);
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(src)) {
        auto left = clone_plan_skeleton(semi->left_, builder, materializer);
        auto right = clone_plan_skeleton(semi->right_, builder, materializer);
        if (left == nullptr || right == nullptr) {
            return nullptr;
        }
        auto dst = std::make_shared<SemiJoinPlan>(left, right, clone_conditions(semi->conds_));
        copy_runtime_cache(src, dst);
        uint32_t id = builder->next_node();
        assign_template_node(dst, id, materializer);
        return dst;
    }
    return nullptr;
}

inline RuntimeNodeKind runtime_kind_for_plan(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return RuntimeNodeKind::kOther;
    }
    switch (plan->tag) {
        case T_SeqScan: return RuntimeNodeKind::kSeqScan;
        case T_IndexScan: return RuntimeNodeKind::kIndexScan;
        case T_NestLoop:
        case T_IndexNestLoop:
        case T_HashJoin:
        case T_SortMerge:
        case T_SemiJoin:
            return RuntimeNodeKind::kJoin;
        case T_CountIndexAggregate: return RuntimeNodeKind::kCountIndex;
        case T_MinMaxIndexAggregate: return RuntimeNodeKind::kMinMaxIndex;
        case T_Aggregate: return RuntimeNodeKind::kAggregate;
        case T_Sort: return RuntimeNodeKind::kSort;
        case T_Limit: return RuntimeNodeKind::kLimit;
        case T_select:
        case T_Insert:
        case T_Update:
        case T_Delete:
        case T_Load:
            return RuntimeNodeKind::kDml;
        default: return RuntimeNodeKind::kOther;
    }
}

inline void attach_runtime_feedback(const std::shared_ptr<Plan> &plan,
                                    const std::shared_ptr<RuntimeFeedbackStore> &store) {
    if (plan == nullptr || store == nullptr) {
        return;
    }
    if (plan->template_node_id_ != kInvalidRuntimeNodeId) {
        plan->runtime_feedback_ =
            store->ensure(plan->template_node_id_, runtime_kind_for_plan(plan));
    }

    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        attach_runtime_feedback(dml->subplan_, store);
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        attach_runtime_feedback(join->left_, store);
        attach_runtime_feedback(join->right_, store);
        return;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        attach_runtime_feedback(projection->subplan_, store);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        attach_runtime_feedback(sort->subplan_, store);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        attach_runtime_feedback(aggregate->subplan_, store);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        attach_runtime_feedback(limit->subplan_, store);
        return;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        attach_runtime_feedback(semi->left_, store);
        attach_runtime_feedback(semi->right_, store);
        return;
    }
    if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto &subplan : union_plan->subplans_) {
            attach_runtime_feedback(subplan, store);
        }
    }
}

inline bool fill_value_from_source(Value *value, const PlanLiteralSlot &slot, const SqlTemplateLiteral &literal,
                                   Context *context = nullptr) {
    return query_template_detail::fill_value_from_literal(value, slot.query_slot, literal, context);
}

class PlanSlotBuilder {
   public:
    PlanSlotBuilder(const std::shared_ptr<Query> &query, const std::vector<query_template_detail::QueryLiteralSlot> &slots,
                    const SqlTemplateCandidate &candidate)
        : query_(query), query_slots_(slots), candidate_(candidate) {
        used_.assign(query_slots_.size(), false);
    }

    bool add_value(PlanLiteralTarget target, uint32_t node, size_t index, const Value &value) {
        return add_by_match(target, node, index, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                     size_t source_index) {
            if (slot.target != query_target_for_plan_target(target)) {
                return false;
            }
            if (target == PlanLiteralTarget::kDmlValue && slot.index < query_->values.size()) {
                return same_literal_value(value, query_->values[slot.index]);
            }
            if (target == PlanLiteralTarget::kDmlSetValue && slot.index < query_->set_clauses.size()) {
                return query_->set_clauses[slot.index].rhs_has_val &&
                       same_literal_value(value, query_->set_clauses[slot.index].rhs);
            }
            if ((target == PlanLiteralTarget::kDmlCondValue || target == PlanLiteralTarget::kScanCondValue ||
                 target == PlanLiteralTarget::kJoinCondValue) &&
                slot.index < query_->conds.size()) {
                return same_literal_value(value, query_->conds[slot.index].rhs_val);
            }
            if (target == PlanLiteralTarget::kSemiCondValue && slot.index < query_->semi_conds.size()) {
                return same_literal_value(value, query_->semi_conds[slot.index].rhs_val);
            }
            (void)source_index;
            return false;
        });
    }

    bool add_condition(PlanLiteralTarget target, uint32_t node, size_t index, const Condition &cond) {
        if (!cond.is_rhs_val) {
            return true;
        }
        return add_by_match(target, node, index, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                     size_t source_index) {
            if (slot.target != query_target_for_plan_target(target)) {
                return false;
            }
            if ((target == PlanLiteralTarget::kDmlCondValue || target == PlanLiteralTarget::kScanCondValue ||
                 target == PlanLiteralTarget::kJoinCondValue) &&
                slot.index < query_->conds.size()) {
                return condition_matches_literal(cond, query_->conds[slot.index]);
            }
            if (target == PlanLiteralTarget::kSemiCondValue && slot.index < query_->semi_conds.size()) {
                return condition_matches_literal(cond, query_->semi_conds[slot.index]);
            }
            (void)source_index;
            return false;
        });
    }

    bool add_set_clause(uint32_t node, size_t index, const SetClause &clause) {
        if (!clause.rhs_has_val) {
            return true;
        }
        return add_by_match(PlanLiteralTarget::kDmlSetValue, node, index,
                            [&](const query_template_detail::QueryLiteralSlot &slot, size_t source_index) {
                                if (slot.target != query_template_detail::QueryLiteralTarget::kUpdateSetValue ||
                                    slot.index >= query_->set_clauses.size()) {
                                    return false;
                                }
                                (void)source_index;
                                return set_clause_matches_literal(clause, query_->set_clauses[slot.index]);
                            });
    }

    bool add_having(uint32_t node, size_t index, const std::shared_ptr<ast::HavingExpr> &having) {
        return add_by_match(PlanLiteralTarget::kHavingValue, node, index,
                            [&](const query_template_detail::QueryLiteralSlot &slot, size_t source_index) {
                                if (slot.target != query_template_detail::QueryLiteralTarget::kHavingValue ||
                                    slot.index >= query_->having_conds.size()) {
                                    return false;
                                }
                                return ast_value_matches_literal(having->rhs, slot, candidate_.literals[source_index]);
                            });
    }

    bool add_limit(PlanLiteralTarget target, uint32_t node, int limit) {
        if (limit < 0 || !query_->has_limit) {
            return true;
        }
        return add_by_match(target, node, 0, [&](const query_template_detail::QueryLiteralSlot &slot,
                                                 size_t source_index) {
            (void)source_index;
            return slot.target == query_template_detail::QueryLiteralTarget::kLimitValue &&
                   query_->limit == limit;
        });
    }

    bool done() const {
        for (size_t i = 0; i < query_slots_.size(); ++i) {
            if (!used_[i]) {
                return false;
            }
        }
        return true;
    }

    std::vector<PlanLiteralSlot> take_slots() { return std::move(slots_); }

   private:
    template <typename Predicate>
    bool add_by_match(PlanLiteralTarget target, uint32_t node, size_t index, Predicate pred) {
        std::vector<size_t> matches;
        std::vector<size_t> unused_matches;
        for (size_t i = 0; i < query_slots_.size(); ++i) {
            if (!pred(query_slots_[i], i)) {
                continue;
            }
            matches.push_back(i);
            if (!used_[i]) {
                unused_matches.push_back(i);
            }
        }
        if (matches.empty()) {
            return false;
        }
        if (unused_matches.size() > 1) {
            return false;
        }
        if (unused_matches.empty() && matches.size() > 1) {
            return false;
        }
        auto source = unused_matches.empty() ? matches.front() : unused_matches.front();
        used_[source] = true;
        PlanLiteralSlot slot;
        slot.target = target;
        slot.plan_node = node;
        slot.index = static_cast<uint32_t>(index);
        slot.source_index = static_cast<uint32_t>(source);
        slot.query_slot = query_slots_[source];
        slots_.push_back(std::move(slot));
        return true;
    }

    const std::shared_ptr<Query> &query_;
    const std::vector<query_template_detail::QueryLiteralSlot> &query_slots_;
    const SqlTemplateCandidate &candidate_;
    std::vector<bool> used_;
    std::vector<PlanLiteralSlot> slots_;
};

inline bool collect_plan_slots(const std::shared_ptr<Plan> &plan, uint32_t *next_node, PlanSlotBuilder *builder) {
    if (plan == nullptr) {
        return true;
    }
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        if (!collect_plan_slots(dml->subplan_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < dml->values_.size(); ++i) {
            if (!builder->add_value(PlanLiteralTarget::kDmlValue, node, i, dml->values_[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < dml->set_clauses_.size(); ++i) {
            if (!builder->add_set_clause(node, i, dml->set_clauses_[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < dml->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kDmlCondValue, node, i, dml->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < scan->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kScanCondValue, node, i, scan->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        if (!collect_plan_slots(join->left_, next_node, builder) ||
            !collect_plan_slots(join->right_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < join->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kJoinCondValue, node, i, join->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        if (!collect_plan_slots(projection->subplan_, next_node, builder)) {
            return false;
        }
        (*next_node)++;
        return true;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        if (!collect_plan_slots(sort->subplan_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        return builder->add_limit(PlanLiteralTarget::kSortLimitValue, node, sort->limit_);
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        if (!collect_plan_slots(aggregate->subplan_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < aggregate->having_conds_.size(); ++i) {
            if (!builder->add_having(node, i, aggregate->having_conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(plan)) {
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < count->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kScanCondValue, node, i, count->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        if (!collect_plan_slots(limit->subplan_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        return builder->add_limit(PlanLiteralTarget::kLimitValue, node, limit->limit_);
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        if (!collect_plan_slots(semi->left_, next_node, builder) ||
            !collect_plan_slots(semi->right_, next_node, builder)) {
            return false;
        }
        uint32_t node = (*next_node)++;
        for (size_t i = 0; i < semi->conds_.size(); ++i) {
            if (!builder->add_condition(PlanLiteralTarget::kSemiCondValue, node, i, semi->conds_[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

inline std::shared_ptr<PlanTemplate> make_template(const SqlTemplateCandidate &candidate,
                                                   const std::shared_ptr<Query> &query,
                                                   const std::shared_ptr<Plan> &plan) {
    if (query == nullptr || plan == nullptr || candidate.literals.size() == 0) {
        return nullptr;
    }
    auto dml = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (dml == nullptr || !cacheable_dml_tag(dml->tag) || query->kind == StmtKind::Union ||
        !query->union_queries.empty()) {
        return nullptr;
    }

    auto query_template = query_template_detail::make_template(candidate, query->parse, query);
    if (query_template == nullptr || query_template->slots.size() != candidate.literals.size()) {
        return nullptr;
    }
    PlanSlotBuilder slot_builder(query, query_template->slots, candidate);
    uint32_t node = 0;
    if (!collect_plan_slots(plan, &node, &slot_builder) || !slot_builder.done()) {
        return nullptr;
    }

    PlanCloneBuilder clone_builder;
    auto skeleton = clone_plan_skeleton(plan, &clone_builder);
    if (skeleton == nullptr) {
        return nullptr;
    }
    auto templ = std::make_shared<PlanTemplate>();
    templ->skeleton = std::move(skeleton);
    templ->slots = slot_builder.take_slots();
    templ->feedback = std::make_shared<RuntimeFeedbackStore>();
    templ->feedback->nodes.resize(clone_builder.node_count());
    attach_runtime_feedback(templ->skeleton, templ->feedback);
    templ->literal_count = candidate.literals.size();
    return templ;
}

inline bool apply_literal_slot(const PlanLiteralSlot &slot, const SqlTemplateLiteral &literal,
                               const PlanMaterializer &materializer, Context *context = nullptr) {
    auto node = materializer.get(slot.plan_node);
    if (node == nullptr) {
        return false;
    }
    switch (slot.target) {
        case PlanLiteralTarget::kDmlValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->values_.size() &&
                   fill_value_from_source(&dml->values_[slot.index], slot, literal, context);
        }
        case PlanLiteralTarget::kDmlSetValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->set_clauses_.size() &&
                   fill_value_from_source(&dml->set_clauses_[slot.index].rhs, slot, literal, context);
        }
        case PlanLiteralTarget::kDmlCondValue: {
            auto dml = std::dynamic_pointer_cast<DMLPlan>(node);
            return dml != nullptr && slot.index < dml->conds_.size() &&
                   fill_value_from_source(&dml->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kScanCondValue: {
            auto scan = std::dynamic_pointer_cast<ScanPlan>(node);
            if (scan != nullptr) {
                return slot.index < scan->conds_.size() &&
                       fill_value_from_source(&scan->conds_[slot.index].rhs_val, slot, literal, context);
            }
            auto count = std::dynamic_pointer_cast<CountIndexAggregatePlan>(node);
            return count != nullptr && slot.index < count->conds_.size() &&
                   fill_value_from_source(&count->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kJoinCondValue: {
            auto join = std::dynamic_pointer_cast<JoinPlan>(node);
            return join != nullptr && slot.index < join->conds_.size() &&
                   fill_value_from_source(&join->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kSemiCondValue: {
            auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(node);
            return semi != nullptr && slot.index < semi->conds_.size() &&
                   fill_value_from_source(&semi->conds_[slot.index].rhs_val, slot, literal, context);
        }
        case PlanLiteralTarget::kHavingValue: {
            auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(node);
            if (aggregate == nullptr || slot.index >= aggregate->having_conds_.size()) {
                return false;
            }
            auto value = query_template_detail::ast_value_from_literal(slot.query_slot, literal);
            if (value == nullptr) {
                return false;
            }
            aggregate->having_conds_[slot.index]->rhs = std::move(value);
            return true;
        }
        case PlanLiteralTarget::kLimitValue: {
            auto limit = std::dynamic_pointer_cast<LimitPlan>(node);
            if (limit == nullptr || literal.type != SqlTemplateLiteralType::kInt) {
                return false;
            }
            limit->limit_ = literal.int_val * slot.query_slot.parsed_slot.numeric_sign;
            return true;
        }
        case PlanLiteralTarget::kSortLimitValue: {
            auto sort = std::dynamic_pointer_cast<SortPlan>(node);
            if (sort == nullptr || literal.type != SqlTemplateLiteralType::kInt) {
                return false;
            }
            sort->limit_ = literal.int_val * slot.query_slot.parsed_slot.numeric_sign;
            return true;
        }
    }
    return false;
}

inline uint64_t estimate_table_capacity_rows(const ScanPlan &scan) {
    if (scan.runtime_cache_ == nullptr || !scan.runtime_cache_->has_table || scan.runtime_cache_->fh == nullptr) {
        return 0;
    }
    RmFileHdr file_hdr = scan.runtime_cache_->fh->get_file_hdr();
    if (file_hdr.num_pages <= RM_FIRST_RECORD_PAGE || file_hdr.num_records_per_page <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(file_hdr.num_pages - RM_FIRST_RECORD_PAGE) *
           static_cast<uint64_t>(file_hdr.num_records_per_page);
}

inline uint64_t estimate_table_data_pages(const ScanPlan &scan) {
    if (scan.runtime_cache_ == nullptr || !scan.runtime_cache_->has_table || scan.runtime_cache_->fh == nullptr) {
        return 0;
    }
    RmFileHdr file_hdr = scan.runtime_cache_->fh->get_file_hdr();
    if (file_hdr.num_pages <= RM_FIRST_RECORD_PAGE) {
        return 0;
    }
    return static_cast<uint64_t>(file_hdr.num_pages - RM_FIRST_RECORD_PAGE);
}

inline bool index_unique_point_lookup(const ScanPlan &scan, const IndexMeta &index) {
    if (!index.unique) {
        return false;
    }
    size_t equality_prefix = 0;
    for (const auto &index_col : index.cols) {
        auto cond_it = std::find_if(scan.conds_.begin(), scan.conds_.end(), [&](const Condition &cond) {
            return cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name == scan.visible_name_ &&
                   cond.lhs_col.col_name == index_col.name;
        });
        if (cond_it == scan.conds_.end()) {
            break;
        }
        equality_prefix++;
    }
    return equality_prefix == static_cast<size_t>(index.col_num);
}

inline bool scan_feedback_prefers_seq_scan(const ScanPlan &scan) {
    if (scan.tag != T_IndexScan || scan.runtime_cache_ == nullptr || !scan.runtime_cache_->has_index ||
        scan.runtime_feedback_ == nullptr) {
        return false;
    }

    auto snapshot = load_scan_feedback_snapshot(scan.runtime_feedback_);
    if (snapshot.executions < kScanFeedbackMinExecutions || snapshot.index_entries == 0) {
        return false;
    }

    const IndexMeta &index = scan.runtime_cache_->index_meta;
    if (index_unique_point_lookup(scan, index)) {
        return false;
    }

    bool covering = !scan.required_cols_.empty() &&
                    index_covers_required_and_conditions(index, scan.required_cols_, scan.conds_, scan.visible_name_);
    if (covering) {
        double fallback_ratio = runtime_feedback_ratio(snapshot.heap_fetches, snapshot.index_entries);
        if (fallback_ratio <= kScanFeedbackGoodCoveringFallbackRatio) {
            return false;
        }
        return false;
    }

    uint64_t table_pages = estimate_table_data_pages(scan);
    if (table_pages > 0 && table_pages <= kScanFeedbackSmallTablePages) {
        return true;
    }

    uint64_t table_rows = estimate_table_capacity_rows(scan);
    if (table_rows == 0 && snapshot.rows_scanned > 0) {
        table_rows = std::max<uint64_t>(1, snapshot.rows_scanned / std::max<uint64_t>(1, snapshot.executions));
    }

    if (table_rows > 0) {
        double avg_index_entries = static_cast<double>(snapshot.index_entries) /
                                   static_cast<double>(snapshot.executions);
        double avg_heap_fetches = static_cast<double>(snapshot.heap_fetches) /
                                  static_cast<double>(snapshot.executions);
        return (avg_index_entries / static_cast<double>(table_rows)) >= kScanFeedbackHighTableRatio ||
               (avg_heap_fetches / static_cast<double>(table_rows)) >= kScanFeedbackHighTableRatio;
    }

    return false;
}

inline void apply_scan_feedback_hints(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return;
    }
    if (auto scan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (scan_feedback_prefers_seq_scan(*scan)) {
            scan->tag = T_SeqScan;
            scan->index_col_names_.clear();
        }
        return;
    }
    if (auto dml = std::dynamic_pointer_cast<DMLPlan>(plan)) {
        apply_scan_feedback_hints(dml->subplan_);
        return;
    }
    if (auto join = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        apply_scan_feedback_hints(join->left_);
        apply_scan_feedback_hints(join->right_);
        return;
    }
    if (auto projection = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        apply_scan_feedback_hints(projection->subplan_);
        return;
    }
    if (auto sort = std::dynamic_pointer_cast<SortPlan>(plan)) {
        apply_scan_feedback_hints(sort->subplan_);
        return;
    }
    if (auto aggregate = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
        apply_scan_feedback_hints(aggregate->subplan_);
        return;
    }
    if (auto limit = std::dynamic_pointer_cast<LimitPlan>(plan)) {
        apply_scan_feedback_hints(limit->subplan_);
        return;
    }
    if (auto semi = std::dynamic_pointer_cast<SemiJoinPlan>(plan)) {
        apply_scan_feedback_hints(semi->left_);
        apply_scan_feedback_hints(semi->right_);
        return;
    }
    if (auto union_plan = std::dynamic_pointer_cast<UnionPlan>(plan)) {
        for (auto &subplan : union_plan->subplans_) {
            apply_scan_feedback_hints(subplan);
        }
    }
}

inline std::shared_ptr<Plan> materialize_template(const PlanTemplate &templ,
                                                  const SqlTemplateCandidate &candidate,
                                                  Context *context = nullptr) {
    PlanCloneBuilder clone_builder;
    PlanMaterializer materializer(templ.feedback == nullptr ? 0 : templ.feedback->nodes.size());
    auto root = clone_plan_skeleton(templ.skeleton, &clone_builder, &materializer);
    if (root == nullptr || templ.slots.empty()) {
        return nullptr;
    }
    materializer.set_root(root);
    if (templ.literal_count != candidate.literals.size() || templ.slots.size() < candidate.literals.size()) {
        return nullptr;
    }
    for (const auto &slot : templ.slots) {
        if (slot.source_index >= candidate.literals.size()) {
            return nullptr;
        }
        if (!apply_literal_slot(slot, candidate.literals[slot.source_index], materializer, context)) {
            return nullptr;
        }
    }
    apply_scan_feedback_hints(root);
    return root;
}

class PlanTemplateCache {
   public:
    explicit PlanTemplateCache(size_t capacity = 256) : capacity_(capacity) {}

    std::shared_ptr<Plan> lookup(const SqlTemplateCandidate &candidate, IsolationLevel isolation_level,
                                 uint64_t epoch, Context *context = nullptr) {
        std::shared_ptr<const PlanTemplate> templ;
        {
            std::lock_guard<std::mutex> guard(latch_);
            PlanTemplateKey key{candidate.key, isolation_level};
            auto it = entries_.find(key);
            if (it == entries_.end()) {
                return nullptr;
            }
            auto list_it = it->second;
            if (list_it->schema_epoch != epoch) {
                lru_.erase(list_it);
                entries_.erase(it);
                return nullptr;
            }
            lru_.splice(lru_.begin(), lru_, list_it);
            templ = lru_.begin()->templ;
        }
        return materialize_template(*templ, candidate, context);
    }

    void store(const SqlTemplateCandidate &candidate, const std::shared_ptr<Query> &query,
               const std::shared_ptr<Plan> &plan, IsolationLevel isolation_level, uint64_t epoch) {
        auto templ = make_template(candidate, query, plan);
        if (templ == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> guard(latch_);
        PlanTemplateKey key{candidate.key, isolation_level};
        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            lru_.erase(existing->second);
            entries_.erase(existing);
        }
        lru_.push_front(CacheEntry{key, epoch, std::move(templ)});
        entries_[key] = lru_.begin();
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
    std::unordered_map<PlanTemplateKey, std::list<CacheEntry>::iterator, PlanTemplateKeyHash> entries_;
};

inline PlanTemplateCache &cache_instance() {
    static PlanTemplateCache cache;
    return cache;
}

}  // namespace plan_template_detail

inline std::shared_ptr<Plan> lookup_plan_template(const SqlTemplateCandidate &candidate,
                                                  IsolationLevel isolation_level, Context *context = nullptr) {
    return plan_template_detail::cache_instance().lookup(candidate, isolation_level, sql_template_schema_epoch(),
                                                         context);
}

inline void store_plan_template(const SqlTemplateCandidate &candidate, const std::shared_ptr<Query> &query,
                                const std::shared_ptr<Plan> &plan, IsolationLevel isolation_level) {
    plan_template_detail::cache_instance().store(candidate, query, plan, isolation_level,
                                                 sql_template_schema_epoch());
}

inline void clear_plan_template_cache() {
    plan_template_detail::cache_instance().clear();
}

inline bool plan_template_cacheable_session(IsolationLevel isolation_level) {
    return isolation_level == IsolationLevel::READ_COMMITTED ||
           isolation_level == IsolationLevel::SNAPSHOT_ISOLATION;
}

}  // namespace rmdb
