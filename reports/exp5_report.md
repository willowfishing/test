# 实验5 实验报告：聚合函数与分组统计

## 实验目标
实现聚合函数（COUNT、MAX、MIN、SUM、AVG）、分组统计（GROUP BY、HAVING）、排序（ORDER BY）和 LIMIT。

## 设计与实现概述
- 聚合算子：AggregationExecutor 实现 COUNT(col/*)。MAX/MIN/SUM/AVG 对 int/float 列
- GROUP BY: 按分组列划分元组，每分组内应用聚合函数
- HAVING: 对分组结果进行条件过滤
- ORDER BY: 多列排序 + ASC/DESC
- LIMIT: 限制结果行数
- 健壮性：非聚合列未在 GROUP BY 中时报 failure

## 说明
本题目的聚合执行器需新增 AggregationExecutor 到 src/execution/ 目录，并在 planner/parser 中添加相应支持。

## 提交文件清单
src/execution/executor_aggregation.h (新增), src/parser/yacc.y, src/analyze/analyze.cpp
