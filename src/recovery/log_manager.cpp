#include <cstring>
#include "log_manager.h"

lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    if (log_record == nullptr) return INVALID_LSN;
    // copy log record to log buffer
    // For now, just assign an LSN
    lsn_t lsn = global_lsn_++;
    log_record->lsn_ = lsn;
    return lsn;
}

void LogManager::flush_log_to_disk() {
    // flush log buffer to disk
}
