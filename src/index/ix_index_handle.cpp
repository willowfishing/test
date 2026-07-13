/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "ix_index_handle.h"

#include <algorithm>

#include "ix_scan.h"

int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        int mid = (left + right) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        int mid = (left + right) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key) - 1;
    if (pos < 0) {
        pos = 0;
    }
    return value_at(pos);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    assert(n >= 0 && get_size() + n <= get_max_size());
    int move_cnt = get_size() - pos;
    if (move_cnt > 0) {
        memmove(get_key(pos + n), get_key(pos), move_cnt * file_hdr->col_tot_len_);
        memmove(get_rid(pos + n), get_rid(pos), move_cnt * sizeof(Rid));
    }
    memcpy(get_key(pos), key, n * file_hdr->col_tot_len_);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    set_size(get_size() + n);
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return get_size();
    }
    insert_pair(pos, key, value);
    return get_size();
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int move_cnt = get_size() - pos - 1;
    if (move_cnt > 0) {
        memmove(get_key(pos), get_key(pos + 1), move_cnt * file_hdr->col_tot_len_);
        memmove(get_rid(pos), get_rid(pos + 1), move_cnt * sizeof(Rid));
    }
    set_size(get_size() - 1);
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                              Transaction *transaction, bool find_first) {
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t child_page_no = find_first ? node->value_at(0) : node->internal_lookup(key);
        PageId old_pid = node->get_page_id();
        buffer_pool_manager_->unpin_page(old_pid, false);
        node = fetch_node(child_page_no);
    }
    return std::make_pair(node, false);
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    result->clear();
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found) {
        result->push_back(*rid);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->set_parent_page_no(node->get_parent_page_no());
    new_node->set_size(0);

    int total = node->get_size();
    int left_size = total / 2;
    int right_size = total - left_size;
    new_node->insert_pairs(0, node->get_key(left_size), node->get_rid(left_size), right_size);
    node->set_size(left_size);

    if (node->is_leaf_page()) {
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(node->get_next_leaf());
        if (node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        node->set_next_leaf(new_node->get_page_no());
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        new_node->set_prev_leaf(IX_NO_PAGE);
        new_node->set_next_leaf(IX_NO_PAGE);
        for (int i = 0; i < new_node->get_size(); ++i) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    if (old_node->is_root_page()) {
        IxNodeHandle *root = create_node();
        root->page_hdr->is_leaf = false;
        root->set_parent_page_no(IX_NO_PAGE);
        root->set_size(0);
        Rid old_rid{old_node->get_page_no(), -1};
        Rid new_rid{new_node->get_page_no(), -1};
        root->insert_pair(0, old_node->get_key(0), old_rid);
        root->insert_pair(1, new_node->get_key(0), new_rid);
        old_node->set_parent_page_no(root->get_page_no());
        new_node->set_parent_page_no(root->get_page_no());
        update_root_page_no(root->get_page_no());
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        return;
    }

    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    Rid new_rid{new_node->get_page_no(), -1};
    int pos = parent->lower_bound(key);
    parent->insert_pair(pos, key, new_rid);
    if (parent->get_size() > parent->get_max_size() - 1) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    int old_size = leaf->get_size();
    int new_size = leaf->insert(key, value);
    if (new_size == old_size) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return -1;
    }
    bool inserted_at_front = leaf->lower_bound(key) == 0;
    if (leaf->get_size() > leaf->get_max_size() - 1) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    } else if (inserted_at_front) {
        maintain_parent(leaf);
    }
    page_id_t page_no = leaf->get_page_no();
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return page_no;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    int old_size = leaf->get_size();
    leaf->remove(key);
    bool removed = leaf->get_size() != old_size;
    if (removed && leaf->get_size() > 0) {
        maintain_parent(leaf);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), removed);
    return removed;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    return false;
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    return false;
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return rid;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->lower_bound(key);
    while (pos >= leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        leaf = fetch_node(next);
        pos = 0;
    }
    Iid iid{leaf->get_page_no(), pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->upper_bound(key);
    while (pos >= leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
        page_id_t next = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        leaf = fetch_node(next);
        pos = 0;
    }
    Iid iid{leaf->get_page_no(), pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid{file_hdr_->last_leaf_, node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    return Iid{file_hdr_->first_leaf_, 0};
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle *IxIndexHandle::create_node() {
    PageId new_page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    file_hdr_->num_pages_ = std::max(file_hdr_->num_pages_, new_page_id.page_no + 1);
    auto node = new IxNodeHandle(file_hdr_, page);
    memset(page->get_data(), 0, PAGE_SIZE);
    node->page_hdr = reinterpret_cast<IxPageHdr *>(page->get_data());
    node->keys = page->get_data() + sizeof(IxPageHdr);
    node->rids = reinterpret_cast<Rid *>(node->keys + file_hdr_->keys_size_);
    node->page_hdr->next_free_page_no = IX_NO_PAGE;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = false;
    node->page_hdr->prev_leaf = IX_NO_PAGE;
    node->page_hdr->next_leaf = IX_NO_PAGE;
    return node;
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE && curr->get_size() > 0) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        if (memcmp(parent->get_key(rank), curr->get_key(0), file_hdr_->col_tot_len_) == 0) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
            break;
        }
        memcpy(parent->get_key(rank), curr->get_key(0), file_hdr_->col_tot_len_);
        PageId parent_pid = parent->get_page_id();
        curr = parent;
        buffer_pool_manager_->unpin_page(parent_pid, true);
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    if (leaf->get_prev_leaf() != IX_LEAF_HEADER_PAGE) {
        IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
        prev->set_next_leaf(leaf->get_next_leaf());
        buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    }
    if (leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
        IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
        next->set_prev_leaf(leaf->get_prev_leaf());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    }
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        IxNodeHandle *child = fetch_node(node->value_at(child_idx));
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
