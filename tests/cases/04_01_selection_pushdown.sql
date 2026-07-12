create table t (a int, b int);
insert into t values (1, 5);
insert into t values (2, 8);
insert into t values (3, 12);
insert into t values (4, 6);
insert into t values (5, 20);
select a, b from t where a > 1 and b < 10;
explain analyze select a, b from t where a > 1 and b < 10;
drop table t;
