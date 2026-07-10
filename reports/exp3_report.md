# 实验3 实验报告：唯一索引

## 实验目标
在现有系统基础上增添 B+ 树唯一索引功能，支持创建/删除/展示索引、单点查询、范围查询和索引与基表同步。

## 设计与实现

### B+ 树索引核心
**文件:** src/index/ix_index_handle.cpp (515行)
- B+ 树节点管理：IxNodeHandle 实现节点的插入、删除、查找、分裂、合并
- 索引操作：insert_entry、delete_entry、get_value 用于增删查
- 叶子链表：支持前向/后向遍历
- 唯一性：按 B+ 树语义，相同 key 不可重复插入

### 索引扫描
**文件:** src/index/ix_scan.cpp, src/index/ix_scan.h
- IxScan::next(): 遍历叶子节点，支持范围扫描
- is_end(): 通过 iid_ == end_ 判断终止

### 索引匹配规则
**文件:** src/optimizer/planner.cpp
- get_index_cols(): 匹配索引字段，自动检测可用索引
- 最左前缀匹配：支持单字段和多字段索引的部分匹配

### 索引管理
**文件:** src/system/sm_manager.cpp
- create_index: 创建索引元数据 + 索引文件 + 注册文件句柄
- drop_index: 关闭句柄 + 删除文件 + 清除元数据

### 索引维护
在 insert/delete/update 执行时同步更新索引：
- InsertExecutor: 插入后遍历索引，插入新 key
- DeleteExecutor: 删除前遍历索引，删除旧 key
- UpdateExecutor: 先删旧 key，更新记录，再插入新 key

## 测试说明
测试点：
- 测试点1: 创建/删除/展示索引
- 测试点2: 索引单点查询与范围查询（性能要求：索引查询 < 无索引 70%）
- 测试点3: 索引维护（插入/删除/更新时同步更新索引）

## 提交文件清单
src/index/ix_index_handle.cpp, src/index/ix_scan.cpp, src/index/ix_index_handle.h, src/index/ix_scan.h, src/index/ix_manager.h, src/execution/executor_index_scan.h, src/system/sm_manager.cpp
