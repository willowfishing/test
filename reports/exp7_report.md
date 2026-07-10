# 实验7 实验报告：嵌套循环连接及其优化

## 实验目标
实现嵌套循环连接（NLJ）和索引嵌套循环连接（INLJ），支持 EXPLAIN ANALYZE 运行时统计。

## 设计与实现

### NLJ (Nested Loop Join)
**文件:** src/execution/executor_nestedloop_join.h
- 双层循环：外表每行驱动内表全量扫描
- 连接条件匹配：compare_col + check_compare 比较函数
- beginTuple/nextTuple: 管理左右迭代状态

### INLJ (Index Nested Loop Join)
当内表连接列有唯一索引时，使用 IndexScan 替代 SeqScan：
- Scan 节点 type 切换为 IndexScan
- using_index 标注索引列
- 性能要求: INLJ 时间 < NLJ 的 50%

### 左深树执行
((T1 JOIN T2) JOIN T3) 形态，plan tree 递归构建

## 测试说明
- test1: 两表全匹配 (20%)
- test2: 两表部分匹配 (20%)
- test3: 两表完全不匹配 (20%)
- test4: 大数据部分匹配 (INLJ vs NLJ 性能比较) (20%)
- test5: 五表等值连接 (20%)

## 提交文件清单
src/execution/executor_nestedloop_join.h, src/execution/executor_index_scan.h, src/optimizer/planner.cpp
