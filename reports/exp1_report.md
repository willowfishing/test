# 实验1 实验报告：存储管理

## 实验目标
实现 RMDB 数据库管理系统的存储管理层，包括磁盘管理器、缓冲池管理器（含 LRU 替换策略）和记录管理器。

## 设计与实现

### 磁盘管理器 (DiskManager)
**文件:** src/storage/disk_manager.cpp
- create_file: 使用 O_CREAT | O_EXCL | O_RDWR 创建文件
- open_file: O_RDWR 打开，维护 path2fd_/fd2path_ 映射，不存在时抛 FileNotFoundError
- close_file: close(fd) 关闭句柄，清除映射
- destroy_file: unlink 删除，检查文件未打开
- write_page: lseek + write，校验写入字节数
- read_page: lseek + read，校验读取字节数

### LRUReplacer
**文件:** src/replacer/lru_replacer.cpp
- victim: 从 LRUlist_ 尾部取出最久未用的 frame
- pin: 从列表中移除该 frame（正在使用，不可被淘汰）
- unpin: 加入列表头部（可被淘汰）
- 使用 std::scoped_lock 保证线程安全

### BufferPoolManager
**文件:** src/storage/buffer_pool_manager.cpp
- find_victim_page: 优先 free_list_，否则调用 replacer_->victim()
- fetch_page: 查 page_table_；命中则 pin_count++；未命中则淘汰 + 读磁盘
- new_page: 通过 disk_manager_->allocate_page() 分配，找 victim frame 初始化
- update_page: 脏页写回磁盘，更新映射，重置 page
- unpin_page: pin_count--，为 0 时 replacer_->unpin()
- delete_page: pin_count=0 时删除，写回磁盘，加入 free_list_
- flush_page / flush_all_pages: 强制刷盘

### 记录管理器 (RmFileHandle / RmScan)
**文件:** src/record/rm_file_handle.cpp, src/record/rm_scan.cpp
- get_record: 获取页面句柄，bitmap 检查，从 slot 读取
- insert_record: 找空闲 slot，写入数据，更新 bitmap
- delete_record: 清除 bitmap，维护空闲页面链表
- update_record: slot 原地更新
- RmScan::next(): 遍历 page_no / slot_no 找下一条有效记录

## 测试结果
| 测试项 | 结果 |
|--------|------|
| LRUReplacerTest.SampleTest | PASS |
| BufferPoolManagerTest.SampleTest | PASS |
| BufferPoolManagerConcurrencyTest | PASS |
| RecordManagerTest.SimpleTest | PASS |

## 提交文件清单
src/storage/disk_manager.cpp, src/storage/buffer_pool_manager.cpp, src/replacer/lru_replacer.cpp, src/record/rm_file_handle.cpp, src/record/rm_scan.cpp, src/common/config.h

## 问题与解决
1. config.h 缺少 #include <string> 导致编译失败，添加后解决
2. open_file 文件不存在时应抛出 FileNotFoundError 而非 UnixError
3. new_page 需通过 disk_manager_->allocate_page() 分配 page_no 并通过参数返回
