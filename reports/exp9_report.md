# 实验9 实验报告：MVCC 与可配置隔离级别

## 实验目标
实现 MVCC 事务级快照隔离（SI）和 Serializable Snapshot Isolation（SER），支持运行时隔离级别配置。

## 设计与实现概述

### SI (Snapshot Isolation)
- MVCC 事务级快照：start_ts / commit_ts 版本可见性
- 版本链管理：保留旧版本供快照读
- 写写冲突检测：两个并发事务不可同时修改同一记录
- 不支持脏读和不可重复读

### SER (Serializable)
- 在 SI 基础上增加 SSI 风格依赖跟踪
- 读集合、谓词读记录、写集合
- rw 反依赖检测：SELECT 检查不可见写入
- SSI 危险结构：两个连续 rw 反依赖时回滚当前事务

### 语法支持
- SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;
- SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;

## 测试说明
测试点：si/InsertTest, si/DirtyReadTest, si/WriteWriteConflictUpdateTest, ser/UnrepeatableReadTest, ser/PhantomReadTest, ser/WriteSkewTest

## 提交文件清单
src/transaction/transaction_manager.cpp, src/transaction/transaction.h, src/execution/executor_*.h
