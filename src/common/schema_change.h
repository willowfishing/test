#pragma once

#include "common/plan_template_cache.h"
#include "common/query_template_cache.h"

namespace rmdb {

inline void invalidate_sql_template_caches() {
    bump_sql_template_schema_epoch();
    clear_query_template_cache();
    clear_plan_template_cache();
}

}  // namespace rmdb
