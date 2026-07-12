create table t469 (id int, grp int, score int, name char(8));
insert into t469 values (1, 10, 80, 'a');
insert into t469 values (2, 10, 60, 'b');
insert into t469 values (3, 20, 90, 'c');
select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
explain analyze select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
drop table t469;
