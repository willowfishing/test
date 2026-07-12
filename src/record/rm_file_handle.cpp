#include "rm_file_handle.h"

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    char* slot = page_handle.get_slot(rid.slot_no);
    memcpy(record->data, slot, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

Rid RmFileHandle::insert_record(char* buf, Context* context) {
    RmPageHandle page_handle = create_page_handle();
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    char* slot = page_handle.get_slot(slot_no);
    memcpy(slot, buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == page_handle.page->get_page_id().page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        }
    }
    int page_no = page_handle.page->get_page_id().page_no;
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
    return Rid{page_no, slot_no};
}

void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    char* slot = page_handle.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1) {
        release_page_handle(page_handle);
    }
    int fd = page_handle.page->get_page_id().fd;
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    // Force flush the data page to disk
    buffer_pool_manager_->flush_all_pages(fd);
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
}

void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    char* slot = page_handle.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < 0 || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(std::to_string(fd_), page_no);
    }
    PageId page_id = {fd_, page_no};
    Page* page = buffer_pool_manager_->fetch_page(page_id);
    if (page == nullptr) { throw PageNotExistError(std::to_string(fd_), page_no); }
    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id = {fd_, INVALID_PAGE_ID};
    Page* page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) { throw InternalError("Failed to create new page"); }
    file_hdr_.num_pages++;
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    page_handle.page_hdr->num_records = 0;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
    return page_handle;
}

RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    int free_page_no = file_hdr_.first_free_page_no;
    PageId page_id = {fd_, free_page_no};
    Page* page = buffer_pool_manager_->fetch_page(page_id);
    if (page == nullptr) { return create_new_page_handle(); }
    RmPageHandle page_handle(&file_hdr_, page);
    file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char*)&file_hdr_, sizeof(file_hdr_));
    return page_handle;
}

void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    int current_page_no = page_handle.page->get_page_id().page_no;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = current_page_no;
}
