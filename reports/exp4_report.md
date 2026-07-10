# 实验4 实验报告：查询优化与执行

## 实验目标
实现查询优化器，支持谓词下推、投影下推，以及 EXPLAIN ANALYZE 计划树输出与运行时统计。

## 设计与实现

### 谓词下推
**文件:** src/optimizer/planner.cpp
- pop_conds(): 将 WHERE 条件按表分类，下推到对应 ScanPlan
- push_conds(): 将连接条件下推到 JoinPlan

### 投影下推
ProjectionPlan 在生成 select 计划时作为根节点，将投影信息下传到执行器。

### EXPLAIN ANALYZE
- 框架已预留 EXPLAIN 语法解析（parser/yacc.y）
- 计划树输出格式：Scan/Filter/Project/Join 节点，tab 缩进，rows=N 统计
- 需要完善 plan 树的递归输出和运行时行数统计

### 计划树结构
**文件:** src/optimizer/plan.h
- ScanPlan, JoinPlan, ProjectionPlan, SortPlan, DMLPlan, DDLPlan
- 支持 T_SeqScan, T_IndexScan, T_NestLoop, T_SortMerge

## 测试说明
测试类型：单表查询、两表连接（谓词下推）、投影下推、多表连接优化

## 提交文件清单
src/optimizer/planner.cpp, src/optimizer/plan.h, src/parser/yacc.y, src/execution/executor_index_scan.h
