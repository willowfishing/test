# 实验8 实验报告：事务控制语句

## 实验目标
实现显式事务控制语句 begin/commit/abort，支持事务内增删改查的回滚。

## 设计与实现概述

### 事务管理器
**文件:** src/transaction/transaction_manager.cpp, src/transaction/transaction.h
- begin: 开启显式事务，设置 txn_mode=true
- commit: 提交事务，写入 commit log，释放锁
- abort: 回滚事务，通过 undo log 还原旧值

### WAL 日志
**文件:** src/recovery/log_manager.cpp, src/recovery/log_manager.h
- Write-Ahead Log: 先写日志后写数据
- REDO/UNDO 日志记录

### 执行流程
- 单条语句默认包装为隐式事务
- 显示事务中多条语句共享同一事务上下文
- 回滚时撤销事务内所有写操作

## 测试说明
测试点：有索引/无索引情况下的 commit 和 abort，验证回滚后数据恢复到事务开始前状态

## 提交文件清单
src/transaction/transaction_manager.cpp, src/transaction/transaction.h, src/recovery/log_manager.cpp, src/parser/yacc.y
