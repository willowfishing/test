/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analyze/analyze.h"
#include "common/context.h"
#include "optimizer/plan.h"

class Planner {
   private:
    SmManager *sm_manager_;
    bool enable_nestedloop_join{true};
    bool enable_sortmerge_join{false};

   public:
    explicit Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}

    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query,
                                     Context *context);

    void set_enable_nestedloop_join(bool set_val) {
        enable_nestedloop_join = set_val;
    }
    void set_enable_sortmerge_join(bool set_val) {
        enable_sortmerge_join = set_val;
    }

   private:
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query,
                                                Context *context);
    std::shared_ptr<Plan> generate_select_plan(const std::shared_ptr<Query> &query,
                                               Context *context);
    std::shared_ptr<Plan> generate_sort_plan(const std::shared_ptr<Query> &query,
                                             std::shared_ptr<Plan> plan);

    bool get_index_cols(const std::string &physical_table,
                        const std::string &qualifier,
                        const std::vector<Condition> &curr_conds,
                        std::vector<std::string> &index_col_names);

    ColType interp_sv_type(ast::SvType sv_type) {
        static const std::map<ast::SvType, ColType> mapping = {
            {ast::SV_TYPE_INT, TYPE_INT},
            {ast::SV_TYPE_FLOAT, TYPE_FLOAT},
            {ast::SV_TYPE_STRING, TYPE_STRING},
        };
        return mapping.at(sv_type);
    }
};
