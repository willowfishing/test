/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t *frame_id)
{
    // C++17 std::scoped_lock
    // 它能够避免死锁发生，其构造函数能够自动进行上锁操作，析构函数会对互斥量进行解锁操作，保证线程安全。
    std::scoped_lock lock{latch_}; //  如果编译报错可以替换成其他lock,获取互斥锁

    // Todo:
    //  利用lru_replacer中的LRUlist_,LRUHash_实现LRU策略
    //  选择合适的frame指定为淘汰页面,赋值给*frame_id

    // 如果列表为空，返回false表示没有可淘汰的帧 if (LRUlist_.empty())
    if (LRUlist_.empty())
    {
        return false;
    }
    // LRU策略：链表尾部保存最久未使用的帧
    frame_id_t victim_frame = LRUlist_.back(); // 选择链表尾部的frame作为victim
    // 1.popback移除链表尾结点
    LRUlist_.pop_back();
    // 2.在hash表中删除该frame
    LRUhash_.erase(victim_frame);
    // 3.将被淘汰的frame_id通过参数返回
    *frame_id = victim_frame;
    return true;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id)
{
    std::scoped_lock lock{latch_};
    // Todo:
    // 固定指定id的frame
    // 在数据结构中移除该frame
    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end())
        return; // 如果该frame不在hash表中，说明该frame已经被固定了，直接返回
    // 如果该frame在hash表中，说明该frame未被固定，需要将其从链表和hash表中删除
    LRUlist_.erase(it->second); // 从链表中删除该frame
    LRUhash_.erase(it);         // 从hash表中删除该frame
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id)
{
    // Todo:
    //  支持并发锁
    //  选择一个frame取消固定
    if (LRUhash_.count(frame_id))
        return; // 如果该frame已经在hash表中，说明该frame已经被取消固定了，直接返回
    if (LRUlist_.size() >= max_size_)
        return; // 如果链表已经满了，说明已经有max_size_个frame被取消固定了，无法再取消固定了，直接返回
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin(); // 在哈希表中记录该帧对用的链表迭代器，便于删除
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() { return LRUlist_.size(); }
