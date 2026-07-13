/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int num_key = page_hdr->num_key;
    for (int i = 0; i < num_key; i++) {
        if (ix_compare(get_key(i), target, file_hdr->col_types_, file_hdr->col_lens_) >= 0) {
            return i;
        }
    }
    return num_key;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int num_key = page_hdr->num_key;
    for (int i = 0; i < num_key; i++) {
        if (ix_compare(get_key(i), target, file_hdr->col_types_, file_hdr->col_lens_) > 0) {
            return i;
        }
    }
    return num_key;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int idx = lower_bound(key);
    if (idx < page_hdr->num_key &&
        ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(idx);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // upper_bound 返回第一个 > key 的位置
    // 那么 key 应该在 idx-1 的孩子子树中
    int idx = upper_bound(key);
    if (idx == 0) {
        // key < 第一个key，在第一个孩子中
        return value_at(0);
    }
    // key 在 idx-1 和 idx 之间，孩子指针在 idx
    return value_at(idx);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    int num_key = page_hdr->num_key;
    // 移动 [pos, num_key) 到 [pos+n, num_key+n)
    int col_tot_len = file_hdr->col_tot_len_;
    // 移动key
    memmove(keys + (pos + n) * col_tot_len,
            keys + pos * col_tot_len,
            (num_key - pos) * col_tot_len);
    // 移动rid
    memmove(rids + pos + n, rids + pos, (num_key - pos) * sizeof(Rid));
    // 复制新key和rid
    memcpy(keys + pos * col_tot_len, key, n * col_tot_len);
    memcpy(rids + pos, rid, n * sizeof(Rid));
    // 更新键数量
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int idx = lower_bound(key);
    // 检查唯一性：如果key已存在则不插入
    if (idx < page_hdr->num_key &&
        ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return page_hdr->num_key;
    }
    insert_pair(idx, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 */
void IxNodeHandle::erase_pair(int pos) {
    int num_key = page_hdr->num_key;
    int col_tot_len = file_hdr->col_tot_len_;
    // 移动 [pos+1, num_key) 到 [pos, num_key-1)
    if (pos + 1 < num_key) {
        memmove(keys + pos * col_tot_len,
                keys + (pos + 1) * col_tot_len,
                (num_key - pos - 1) * col_tot_len);
        memmove(rids + pos, rids + pos + 1, (num_key - pos - 1) * sizeof(Rid));
    }
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对
 */
int IxNodeHandle::remove(const char *key) {
    int idx = lower_bound(key);
    if (idx < page_hdr->num_key &&
        ix_compare(get_key(idx), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(idx);
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    std::vector<char> buf(PAGE_SIZE, 0);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf.data(), PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf.data());

    // New pages must continue after the pages already persisted in this index.
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    page_id_t root_page_no = file_hdr_->root_page_;
    if (root_page_no == IX_NO_PAGE) {
        return std::make_pair(nullptr, false);
    }
    IxNodeHandle *node = fetch_node(root_page_no);
    while (!node->is_leaf_page()) {
        page_id_t child_page_no = node->internal_lookup(key);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = fetch_node(child_page_no);
    }
    return std::make_pair(node, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, transaction, false);
    if (leaf == nullptr) return false;
    Rid *value = nullptr;
    bool found = leaf->leaf_lookup(key, &value);
    if (found && result != nullptr) {
        result->push_back(*value);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->page_hdr->parent;

    int total_keys = node->get_size();
    int col_tot_len = file_hdr_->col_tot_len_;

    if (node->is_leaf_page()) {
        // 叶子节点分裂：左半保留，右半移到新节点
        int left_keys = total_keys / 2;
        int right_keys = total_keys - left_keys;

        memcpy(new_node->keys, node->get_key(left_keys), right_keys * col_tot_len);
        memcpy(new_node->rids, node->get_rid(left_keys), right_keys * sizeof(Rid));
        new_node->set_size(right_keys);
        node->set_size(left_keys);

        // 更新叶子链表
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());
        if (new_node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(new_node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // 内部节点分裂：中间key升到父节点，不在子节点中保留
        // 调用者应该先保存分隔key = node->get_key(total_keys/2)
        int mid = total_keys / 2;
        node->set_size(mid);
        int right_keys = total_keys - mid - 1;
        if (right_keys > 0) {
            memcpy(new_node->keys, node->get_key(mid + 1), right_keys * col_tot_len);
        }
        memcpy(new_node->rids, node->get_rid(mid + 1), (right_keys + 1) * sizeof(Rid));
        new_node->set_size(right_keys);
        for (int i = 0; i <= right_keys; i++) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

/**
 * @brief Insert separator key and right child into an internal page.
 *
 * Internal pages contain n keys and n+1 child pointers. Splitting child i
 * therefore inserts key[i] and child[i+1], not a key/rid pair at the same
 * array index.
 */
void IxIndexHandle::insert_internal_after_child(IxNodeHandle *parent, int child_idx, const char *key,
                                                const Rid &new_child) {
    const int old_size = parent->get_size();
    const int key_len = file_hdr_->col_tot_len_;
    assert(child_idx >= 0 && child_idx <= old_size);

    memmove(parent->get_key(child_idx + 1), parent->get_key(child_idx),
            static_cast<size_t>(old_size - child_idx) * key_len);
    memmove(parent->get_rid(child_idx + 2), parent->get_rid(child_idx + 1),
            static_cast<size_t>(old_size - child_idx) * sizeof(Rid));
    memcpy(parent->get_key(child_idx), key, key_len);
    parent->set_rid(child_idx + 1, new_child);
    parent->set_size(old_size + 1);
}

/**
 * @brief Insert key & value pair into internal page after split
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->set_size(1);
        memcpy(new_root->get_key(0), key, file_hdr_->col_tot_len_);
        new_root->set_rid(0, {old_node->get_page_no(), -1});
        new_root->set_rid(1, {new_node->get_page_no(), -1});
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        update_root_page_no(new_root->get_page_no());
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    const int child_idx = parent->find_child(old_node);
    insert_internal_after_child(parent, child_idx, key, {new_node->get_page_no(), -1});
    new_node->set_parent_page_no(parent->get_page_no());

    if (parent->get_size() >= parent->get_max_size()) {
        std::vector<char> separator(file_hdr_->col_tot_len_);
        memcpy(separator.data(), parent->get_key(parent->get_size() / 2), file_hdr_->col_tot_len_);
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, separator.data(), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

/**
 * @brief 将指定键值对插入到B+树中
 * @return inserted leaf page number, or IX_NO_PAGE when the unique key exists
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::INSERT, transaction, false);
    if (leaf == nullptr) return IX_NO_PAGE;

    const int pos = leaf->lower_bound(key);
    if (pos < leaf->get_size() &&
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) == 0) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return IX_NO_PAGE;
    }

    leaf->insert_pair(pos, key, value);
    const page_id_t page_no = leaf->get_page_no();

    // Inserting a new minimum changes the separator of this subtree.
    if (pos == 0 && !leaf->is_root_page()) {
        maintain_parent(leaf);
    }

    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        std::vector<char> separator(file_hdr_->col_tot_len_);
        memcpy(separator.data(), new_leaf->get_key(0), file_hdr_->col_tot_len_);
        insert_into_parent(leaf, separator.data(), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return page_no;
}

/**
 * @brief Delete one unique key.
 *
 * Keeping underfull pages is intentionally safer than applying the old merge
 * code, which mixed the leaf key/rid representation with the internal
 * n-keys/n+1-children representation. Search remains correct; empty leaves are
 * skipped by bound scans and can be reused by later inserts.
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::DELETE, transaction, false);
    if (leaf == nullptr) return false;

    const int pos = leaf->lower_bound(key);
    if (pos >= leaf->get_size() ||
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) != 0) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return false;
    }

    leaf->erase_pair(pos);
    if (pos == 0 && leaf->get_size() > 0 && !leaf->is_root_page()) {
        maintain_parent(leaf);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    // 如果是根节点
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    // 如果结点键数量 >= 最小值，不需要操作
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    // 获取父结点
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);

    // 寻找兄弟结点，优先前驱
    int neighbor_index = (index > 0) ? index - 1 : index + 1;
    IxNodeHandle *neighbor = fetch_node(parent->value_at(neighbor_index));

    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        // 重分配
        redistribute(neighbor, node, parent, index);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return false;
    } else {
        // 合并
        return coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 内部节点且只有1个key，将孩子作为新根
        page_id_t new_root_page_no = old_root_node->value_at(0);
        IxNodeHandle *new_root = fetch_node(new_root_page_no);
        new_root->set_parent_page_no(IX_NO_PAGE);
        update_root_page_no(new_root_page_no);
        file_hdr_->first_leaf_ = new_root_page_no;
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);

        // 释放旧根
        release_node_handle(*old_root_node);
        return true;
    } else if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 叶子节点且为空
        update_root_page_no(IX_NO_PAGE);
        file_hdr_->first_leaf_ = IX_LEAF_HEADER_PAGE;
        file_hdr_->last_leaf_ = IX_LEAF_HEADER_PAGE;
        release_node_handle(*old_root_node);
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    int col_tot_len = file_hdr_->col_tot_len_;

    if (index == 0) {
        // node在左边, neighbor在右边
        // 从neighbor移动第一个key到node的末尾
        node->insert_pairs(node->get_size(), neighbor_node->get_key(0), neighbor_node->get_rid(0), 1);
        neighbor_node->erase_pair(0);

        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }

        // 更新parent中指向neighbor的key
        memcpy(parent->get_key(index), neighbor_node->get_key(0), col_tot_len);
    } else {
        // neighbor在左边, node在右边 (index > 0)
        // 从neighbor移动最后一个key到node的开头
        int last = neighbor_node->get_size() - 1;
        node->insert_pairs(0, neighbor_node->get_key(last), neighbor_node->get_rid(last), 1);
        neighbor_node->erase_pair(last);

        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }

        // 更新parent中指向node的key
        memcpy(parent->get_key(index), node->get_key(0), col_tot_len);
    }
}

/**
 * @brief 合并函数
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 确保neighbor是左结点，node是右结点
    if (index == 0) {
        std::swap(*neighbor_node, *node);
        index = 1;
    }

    int col_tot_len = file_hdr_->col_tot_len_;
    int left_size = (*neighbor_node)->get_size();
    int right_size = (*node)->get_size();

    // 将右结点的键值对移到左结点
    memcpy((*neighbor_node)->get_key(left_size), (*node)->get_key(0), right_size * col_tot_len);
    memcpy((*neighbor_node)->get_rid(left_size), (*node)->get_rid(0), right_size * sizeof(Rid));
    (*neighbor_node)->set_size(left_size + right_size);

    // 更新被移动孩子节点的父指针
    if (!(*node)->is_leaf_page()) {
        for (int i = 0; i < right_size; i++) {
            maintain_child(*neighbor_node, left_size + i);
        }
    }

    if ((*node)->is_leaf_page()) {
        // 更新叶子链表
        (*neighbor_node)->set_next_leaf((*node)->get_next_leaf());
        if ((*node)->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node((*node)->get_next_leaf());
            next->set_prev_leaf((*neighbor_node)->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        // 更新last_leaf
        if (file_hdr_->last_leaf_ == (*node)->get_page_no()) {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
        erase_leaf(*node);
    }

    // 从父结点删除node对应的键值对
    int parent_key_idx = index;
    (*parent)->erase_pair(parent_key_idx);

    // 删除node结点
    release_node_handle(**node);
    buffer_pool_manager_->unpin_page((*neighbor_node)->get_page_id(), true);

    // 递归处理父结点
    bool parent_deleted = coalesce_or_redistribute(*parent, transaction, root_is_latched);

    buffer_pool_manager_->unpin_page((*parent)->get_page_id(), !parent_deleted);
    return false;
}

/**
 * @brief 这里把iid转换成了rid
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr, true);
    if (leaf == nullptr) return leaf_end();

    int slot_no = leaf->lower_bound(key);
    page_id_t page_no = leaf->get_page_no();
    while (slot_no == leaf->get_size() && page_no != file_hdr_->last_leaf_) {
        const page_id_t next_page = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        leaf = fetch_node(next_page);
        page_no = next_page;
        slot_no = 0;
    }
    Iid result{page_no, slot_no};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return result;
}

/**
 * @brief FindLeafPage + upper_bound
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr, false);
    if (leaf == nullptr) return leaf_end();

    int slot_no = leaf->upper_bound(key);
    page_id_t page_no = leaf->get_page_no();
    while (slot_no == leaf->get_size() && page_no != file_hdr_->last_leaf_) {
        const page_id_t next_page = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        leaf = fetch_node(next_page);
        page_no = next_page;
        slot_no = 0;
    }
    Iid result{page_no, slot_no};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return result;
}

Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid{file_hdr_->last_leaf_, node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    page_id_t page_no = file_hdr_->first_leaf_;
    IxNodeHandle *node = fetch_node(page_no);
    while (node->get_size() == 0 && page_no != file_hdr_->last_leaf_) {
        const page_id_t next_page = node->get_next_leaf();
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        page_no = next_page;
        node = fetch_node(page_no);
    }
    Iid iid{page_no, 0};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;
    PageId new_page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    memset(page->get_data(), 0, PAGE_SIZE);
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = false;
    node->page_hdr->prev_leaf = IX_NO_PAGE;
    node->page_hdr->next_leaf = IX_NO_PAGE;
    return node;
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    if (node->get_size() == 0) return;

    std::vector<char> subtree_min(file_hdr_->col_tot_len_);
    memcpy(subtree_min.data(), node->get_key(0), file_hdr_->col_tot_len_);
    page_id_t parent_page = node->get_parent_page_no();
    page_id_t child_page = node->get_page_no();

    while (parent_page != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(parent_page);
        int child_rank = -1;
        for (int i = 0; i <= parent->get_size(); ++i) {
            if (parent->value_at(i) == child_page) {
                child_rank = i;
                break;
            }
        }
        assert(child_rank >= 0);

        if (child_rank > 0) {
            memcpy(parent->get_key(child_rank - 1), subtree_min.data(), file_hdr_->col_tot_len_);
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            return;
        }

        child_page = parent->get_page_no();
        parent_page = parent->get_parent_page_no();
        buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
