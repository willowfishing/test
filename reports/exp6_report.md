# 实验6 实验报告：Union 集合算子

## 实验目标
实现 Union 算子，支持多分支查询合并去重、整体 ORDER BY 排序、派生表语法，以及类型系统兼容性检查。

## 设计与实现概述
- Union 算子：遍历所有子节点，收集全部元组，哈希去重后返回
- ORDER BY: 作用于 Union 最终输出，支持 ASC/DESC 多列排序
- 派生表语法：SELECT * FROM (SELECT ... UNION SELECT ...) AS alias ORDER BY ...
- 类型兼容性：列数一致、类型匹配、INT+FLOAT->FLOAT、CHAR(n)+CHAR(m)->CHAR(max(n,m))
- 错误处理：列数不匹配/类型不兼容/ORDER BY 未知列 -> failure

## 提交文件清单
src/execution/executor_union.h (新增), src/parser/yacc.y, src/analyze/analyze.cpp
