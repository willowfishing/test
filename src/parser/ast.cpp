/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#include "ast.h"

namespace ast {

std::shared_ptr<TreeNode> parse_tree;

// Union map: populated during parsing, consumed by analyzer
static std::map<std::string, std::shared_ptr<UnionStmt>> g_union_map;

std::map<std::string, std::shared_ptr<UnionStmt>>& get_union_map() {
    return g_union_map;
}

void clear_union_map() {
    g_union_map.clear();
}

}
