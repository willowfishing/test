/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#undef NDEBUG

#include <cassert>

#include "parser.h"

int main() {
    std::vector<std::string> sqls = {
        "show tables;",
        "desc tb;",
        "create table tb (a int, b float, c char(4));",
        "drop table tb;",
        "create index tb(a);",
        "create index tb(a, b, c);",
        "drop index tb(a, b, c);",
        "drop index tb(b);",
        "insert into tb values (1, 3.14, 'pi');",
        "delete from tb where a = 1;",
        "update tb set a = 1, b = 2.2, c = 'xyz' where x = 2 and y < 1.1 and z > 'abc';",
        "update tb set a=a+5 where a<3;",
        "set transaction isolation level snapshot isolation;",
        "set transaction isolation level serializable;",
        "set transaction isolation level snapshot isolation",
        "set transaction isolation level snapshot;",
        "set session transaction isolation level serializable;",
        "set session characteristics as transaction isolation level snapshot isolation;",
        "set transaction isolation level snapshot_isolation;",
        "set transaction isolation level ser;",
        std::string("\xEF\xBB\xBF") + "set transaction isolation level serializable;",
        "select * from tb;",
        "select * from tb where x <> 2 and y >= 3. and z <= '123' and b < tb.a;",
        "select x.a, y.b from x, y where x.a = y.b and c = d;",
        "select x.a, y.b from x join y where x.a = y.b and c = d;",
        "select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id;",
        "select c.name from customers as c, orders as o where c.customer_id = o.customer_id;",
        "explain analyze select a, b from t where a > 1 and b < 10;",
        "explain analyze select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;",
        "select max(a) as max_a, min(b), sum(b), avg(b), count(*), count(c) from tb;",
        "select a, count(*) as n from tb where b > 0 group by a having count(*) > 1 order by n desc, a asc limit 5;",
        "exit;",
        "help;",
        "create static_checkpoint;",
        "",
    };
    for (auto &sql : sqls) {
        std::cout << sql << std::endl;
        YY_BUFFER_STATE buf = yy_scan_string(sql.c_str());
        assert(yyparse() == 0);
        if (ast::parse_tree != nullptr) {
            ast::TreePrinter::print(ast::parse_tree);
            yy_delete_buffer(buf);
            std::cout << std::endl;
        } else {
            std::cout << "exit/EOF" << std::endl;
        }
    }
    ast::parse_tree.reset();
    return 0;
}
