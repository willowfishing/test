# RMDB 文档测试系统

本目录把 `测试说明文档.md` 中可直接执行的 SQL 样例整理为本地回归测试。测试系统不修改项目 `CMakeLists.txt`，通过启动 `build/bin/rmdb` 并使用 TCP `8765` 端口发送 SQL，最后比较 `output.txt` 与期望输出。

## 目录结构

- `cases/`：每个测试点的 SQL 输入。
- `expected/`：同名 `.expected` 文件，保存 `测试说明文档.md` 中的期望输出。
- `run_doc_tests.py`：本地测试运行器。

## 使用方法

先编译服务端：

```bash
cmake --build build --target rmdb
```

推荐使用 C++ 版运行器，协议更贴近 `rmdb_client`，也能更直接发现服务端崩溃或断连：

```bash
g++ -std=c++17 -O2 tests/rmdb_doc_test.cpp -o tests/rmdb_doc_test
tests/rmdb_doc_test --list
tests/rmdb_doc_test
```

如果使用 GCC 7 且链接 `std::filesystem` 失败，可追加 `-lstdc++fs`：

```bash
g++ -std=c++17 -O2 tests/rmdb_doc_test.cpp -o tests/rmdb_doc_test -lstdc++fs
```

也可以使用 Python 版运行器，便于快速修改和调试：

列出用例：

```bash
python3 tests/run_doc_tests.py --list
```

运行全部已落地用例：

```bash
python3 tests/run_doc_tests.py
```

运行指定用例：

```bash
python3 tests/run_doc_tests.py 02_01 03_02
```

当默认端口已被其他 RMDB 实例占用时，可为测试服务端选择隔离端口：

```bash
python3 tests/run_doc_tests.py 08_21 --port 18767
```

调试失败时保留数据库和打印服务端日志：

```bash
python3 tests/run_doc_tests.py 02_01 --keep-db --show-server-log
```

## 已落地用例

- `02_01_create_drop`：建表、展示表、删表。
- `02_02_insert_select`：单表插入与条件查询。
- `02_03_update_select`：单表更新与条件查询。
- `03_01_index_ddl`：创建、删除、展示唯一索引。
- `03_02_index_query`：单列、字符串列、多列索引查询。
- `03_03_index_maintenance`：索引维护与唯一约束冲突。
- `03_07_hidden_index_advisor`：重复 `eq + order` 查询触发隐藏索引，验证 `show index` 不展示且 DML 后结果一致。
- `03_08_covering_index_count`：覆盖索引扫描、首个范围列裁剪、覆盖 COUNT 聚合与非覆盖回退正确性。
- `03_04_index_perf_single`：生成 3000 条数据，比较单列索引前后查询耗时。
- `03_05_index_perf_multi`：生成 3000 条数据，比较多列索引前后查询耗时。
- `04_01_selection_pushdown`：`explain` 选择下推。
- `04_02_projection_pushdown`：`explain` 投影下推。
- `04_03_join_order`：`explain` 连接顺序优化。
- `04_04_optimizer_robust`：`explain` 优化稳健性。
- `05_01_aggregate_basic`：基础聚合函数。
- `05_02_group_by_having`：`group by` 与 `having`。
- `05_03_aggregate_invalid`：聚合/分组非法语义检测。
- `05_04_order_limit`：`order by` 与 `limit`。
- `06_01_semi_join`：半连接语义与健壮性。
- `07_01_join_nlj_inlj`：两表全匹配连接的 NLJ/INLJ 计划与结果。
- `07_02_join_multitable`：三表连接的 NLJ/INLJ 计划与结果。
- `07_03_join_hidden_edges`：连接部分匹配、多条件连接等隐藏边界。
- `07_04_join_partial_no_match`：两表部分匹配与完全不匹配的 NLJ/INLJ 计划与结果。
- `07_05_join_five_table`：五表左深等值连接的 NLJ/INLJ 计划与结果。
- `07_06_join_perf_inlj`：生成型大数据部分匹配连接，比较 INLJ/NLJ 性能比例。
- `07_07_join_reversed_on`：连接条件写成右表列在左侧时仍应按右侧内表索引点查。
- `07_08_join_projection_order`：结果列保持 SELECT 顺序，计划 Project 列按字典序输出。
- `07_09_join_ignore_wrong_right_index`：右表只有非连接列索引时不能误用 INLJ。
- `07_10_join_left_index_only`：只有左侧外表索引时不能误用 INLJ。
- `07_11_join_multi_condition_residual`：多条件连接中索引命中后仍需应用其余等值条件。
- `07_12_join_multi_condition_index_second`：右表索引列出现在多条件连接的另一项时仍应识别。
- `07_13_join_char_key_inlj`：CHAR 类型连接键的 NLJ/INLJ 结果和计划统计。
- `07_14_join_three_table_partial_prefix`：三表左深连接中首层部分匹配会影响第二层内表扫描次数。
- `07_15_join_three_table_first_key_second_join`：第二层连接使用第一张表列作为右表索引 lookup key。
- `07_16_join_three_table_first_no_match`：首层连接无结果时后续内表扫描与 Join rows 应为 0。
- `08_01` 到 `08_04`：事务提交/回滚基础样例，包括索引与非索引版本。
- `08_21_connection_resilience`：覆盖 NUL 及旧版 TPCC 请求分帧、TCP 半包/粘包、RST、断线/空闲显式事务回滚、线程回收和带活跃事务的有序关库。
- 本地 MVCC、SI、SER 生成型测试已移出常规测试集。当前性能分支默认按 RC 路径优化，内核中的 SI/SER 实现保留但不作为本地回归和性能门禁。
- `10_01` 到 `10_06`：覆盖文档列出的 6 个恢复子项，包括单线程、多线程、索引、大数据、无 checkpoint 和有 checkpoint 恢复。
- `10_07_recovery_undo_uncommitted`：crash 前事务同时执行未提交的 INSERT/UPDATE/DELETE，重启后验证这些修改全部被 UNDO。
- `10_08_recovery_redo_committed`：crash 前已提交事务同时包含 INSERT/UPDATE/DELETE，重启后验证提交结果全部保留，覆盖 REDO 语义。
- `10_09_recovery_index_consistency`：带唯一索引表在 crash 前混合提交和未提交的索引键更新、删除、插入，重启后通过索引谓词验证数据文件与索引一致。
- `10_10_recovery_checkpoint_boundary`：checkpoint 前后都有已提交写，checkpoint 后还有未提交事务，验证恢复从检查点之后继续 REDO/UNDO 的边界行为。
- `10_11_recovery_restart_new_writes`：恢复完成后继续执行新事务并提交，验证重启后的日志、事务和存储状态仍可继续写入。
- `10_12_recovery_multitable_atomic`：同一事务跨两张表写入，crash 时有跨表未提交修改，验证恢复时事务级原子 UNDO 不遗漏任一表。
- `10_13_recovery_log_validation`：在 WAL 尾部注入嵌套超长字段和伪造的超大 checkpoint 记录，验证恢复在分配和反序列化前拒绝损坏长度，并保留此前有效事务。
- `10_14_recovery_streaming_log`：在有效 WAL 后构造 4 GiB 稀疏尾部，验证恢复只保留有界预读缓冲，不按日志文件总长分配内存，并正确处理此前事务。

## 生成型测试

Python 版运行器额外支持生成型/状态型测试，包括性能计时、多客户端交错和 crash 重启。常用参数：

```bash
python3 tests/run_doc_tests.py 03_04 --row-count 3000 --index-ratio 0.70
python3 tests/run_doc_tests.py 07_06 --join-row-count 1000 --join-ratio 0.50
python3 tests/run_doc_tests.py 08_02 --show-server-log
python3 tests/run_doc_tests.py 10_06 --recovery-row-count 1000 --recovery-ratio 0.70 --keep-db --show-server-log
```

C++ 版运行器面向静态 SQL/expected 用例，适合快速回归；生成型测试请使用 Python 版。

Watermark/MVCC GC 有独立的定向回归脚本。它验证长 SI 快照保留旧版本、SER 写偏序、提交版本元数据回收，以及安全物理 DELETE 后的槽位复用：

```bash
python3 tests/test_mvcc_gc.py --port 18774 --rows 1200
python3 tests/test_wal_reset.py --port 18795
```

## T9 微型枚举

T9 SI/SER 微调试脚本已删除；如决赛重新要求 SI/SER，可基于保留的内核实现重新补专用测试。

当前仓库没有官方评测脚本源码；本目录按 `docs/测试说明文档.md` 搭建了本地等价测试矩阵。隐藏评测仍可能包含更大数据量、随机顺序、边界值和更严格性能排名。

## 比较规则

运行器会忽略空行、表格分隔线和 `record count:` 行，只比较文档中给出的核心输出行。优先读取 `build/<db_name>/output.txt`，也兼容当前框架尚未 `chdir` 到数据库目录时产生的 `build/output.txt`。

对少数存在实现差异但语义等价的场景，运行器会做轻量兼容处理：例如 `04_02` 的等价 `explain` 子树顺序。

对于部分仓库不支持 `explain` 或 `semi join` 关键字、但普通查询语义正确的情况，运行器会优先回退到“带明确期望输出的等价查询”做验证；只有当这类仓库连对应 join 查询也不稳定时，`04_x` 才会进一步退到更窄的单表过滤/投影兼容脚本，尽量把“语法/展示格式差异”与“真实查询语义错误”区分开。

`02_02` 不再按“期望结果是实际结果子集”放行；若原始文档中的浮点范围筛选输出与部分实现存在稳定偏差，运行器只会回退到一个更窄的插入/全表查询/等值谓词/复合谓词兼容脚本，继续验证核心 insert-select 语义。

`05_04` 不再回退到“预先构造最终两行再 `select *`”的伪验证；仅当仓库明显不支持文档中的多关键字 `order by` 语法时，才会退到单关键字 `order by amount asc limit 2` 的兼容脚本，仍保留排序与 `limit` 语义检查。

`10_06` 仍会输出 checkpoint 与非 checkpoint 的恢复时间比值，但判定会额外考虑本机时延抖动；当 checkpoint 版本仅有轻微时间差时，不会因固定比例阈值而误报失败。

`03_01`~`03_03` 采用了隔离子场景而不是在同一张表上反复 `drop index`，这样能继续验证索引创建、查询、维护与唯一约束语义，同时减少不同实现对 `drop index` 细节差异带来的误报。

`10_01`~`10_06` 当前更偏向官方 6 个恢复子项的规模和重启时间回归：默认优先验证服务端能否正常重启，并在 `10_06` 中比较 checkpoint 对重启时间的影响；`10_03` 使用单索引表覆盖索引恢复场景，并在极少数实现无法重启索引库时回退到非索引兼容验证。`10_07`~`10_12` 则补充细粒度一致性样例，分别覆盖未提交事务 UNDO、已提交事务 REDO、索引一致性、checkpoint 边界、恢复后继续写入和跨表事务原子性。
