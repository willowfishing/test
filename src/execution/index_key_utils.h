#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "system/sm_meta.h"

inline std::vector<char> make_index_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

inline std::string index_key_bytes(const std::vector<char> &key) {
    return std::string(key.data(), key.size());
}

inline std::uint64_t rid_identity(const Rid &rid) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rid.page_no)) << 32U) |
           static_cast<std::uint32_t>(rid.slot_no);
}
