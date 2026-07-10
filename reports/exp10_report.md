# 实验10 实验报告：基于静态检查点的故障恢复

## 实验目标
实现 WAL 机制、REDO/UNDO 日志、静态检查点创建与基于检查点的系统故障恢复。

## 设计与实现概述

### 日志管理器
**文件:** src/recovery/log_manager.cpp, src/recovery/log_manager.h
- WAL 机制：先写日志后写数据
- 日志缓冲区 + 刷盘策略
- REDO 日志：记录数据修改后的新值
- UNDO 日志：记录数据修改前的旧值

### 静态检查点
- create static_checkpoint 命令
- 步骤：停止新事务 -> 刷日志 -> 写检查点记录 -> 刷脏页 -> 更新重启文件

### 故障恢复
- 不带检查点：从第一条日志开始扫描
- 带检查点：从最新检查点开始扫描，undo list + redo list
- 恢复完成后数据库恢复一致性状态
- 性能要求：带检查点恢复时间 < 不带检查点的 70%

## 测试说明
测试点：单线程/多线程/大数据的 crash recovery，以及带检查点的性能对比

## 提交文件清单
src/recovery/log_manager.cpp, src/recovery/log_recovery.cpp, src/recovery/log_manager.h, src/recovery/log_recovery.h
