create table t266 (id int, score int);
insert into t266 values (1, 90);
insert into t266 values (2, 80);
insert into t266 values (3, 70);
select id from t266 order by score desc limit 1;
explain analyze select id from t266 order by score desc limit 1;
drop table t266;
