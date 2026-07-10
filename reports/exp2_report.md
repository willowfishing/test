# 实验2 实验报告：查询执行

## 实验目标
在存储管理基础上实现 SQL 查询执行引擎，支持 DDL、DML、DQL 及连接查询。

## 设计与实现

### 执行算子
- **SeqScanExecutor**: 通过 RmScan 遍历表记录，返回匹配记录
- **InsertExecutor**: 填充 record buffer 后调用 fh_->insert_record()，同步更新索引
- **DeleteExecutor**: 先删除索引项，再调用 fh_->delete_record()
- **UpdateExecutor**: 删除旧索引项 -> 构建新记录 -> 更新记录 -> 插入新索引项
- **ProjectionExecutor**: 从子节点记录中选取需要的列
- **NestedLoopJoinExecutor**: 双层循环遍历，匹配连接条件后返回合并记录

### 系统管理 (SmManager)
**文件:** src/system/sm_manager.cpp
- create_table: 创建表元数据 + 记录文件 + 注册文件句柄
- drop_table: 关闭文件句柄 + 删除文件 + 清理索引 + 清除元数据
- create_index / drop_index: 管理索引生命周期
- show_tables: 输出表列表到 output.txt
- open_db / close_db: 加载/保存数据库元数据，恢复表文件和索引文件句柄

### 测试说明
本实验通过 SQL 语句进行测试，输出写入 output.txt。测试点包括建表、单表CRUD、条件查询和连接查询。

## 提交文件清单
src/execution/executor_seq_scan.h, src/execution/executor_projection.h, src/execution/executor_delete.h, src/execution/executor_update.h, src/execution/executor_nestedloop_join.h, src/system/sm_manager.cpp
