#include "rm_scan.h"
#include "rm_file_handle.h"

RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;
    if (rid_.page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);
        rid_.slot_no = Bitmap::first_bit(true, ph.bitmap,
                                          file_handle_->file_hdr_.num_records_per_page);
        file_handle_->buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
        while (rid_.slot_no == file_handle_->file_hdr_.num_records_per_page) {
            rid_.page_no++;
            if (rid_.page_no >= file_handle_->file_hdr_.num_pages) { break; }
            RmPageHandle ph2 = file_handle_->fetch_page_handle(rid_.page_no);
            rid_.slot_no = Bitmap::first_bit(true, ph2.bitmap,
                                              file_handle_->file_hdr_.num_records_per_page);
            file_handle_->buffer_pool_manager_->unpin_page(ph2.page->get_page_id(), false);
        }
    }
}

void RmScan::next() {
    RmPageHandle ph = file_handle_->fetch_page_handle(rid_.page_no);
    rid_.slot_no = Bitmap::next_bit(true, ph.bitmap,
                                     file_handle_->file_hdr_.num_records_per_page,
                                     rid_.slot_no);
    file_handle_->buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
    while (rid_.slot_no == file_handle_->file_hdr_.num_records_per_page) {
        rid_.page_no++;
        if (rid_.page_no >= file_handle_->file_hdr_.num_pages) { break; }
        RmPageHandle ph2 = file_handle_->fetch_page_handle(rid_.page_no);
        rid_.slot_no = Bitmap::first_bit(true, ph2.bitmap,
                                          file_handle_->file_hdr_.num_records_per_page);
        file_handle_->buffer_pool_manager_->unpin_page(ph2.page->get_page_id(), false);
    }
}

bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

Rid RmScan::rid() const { return rid_; }
