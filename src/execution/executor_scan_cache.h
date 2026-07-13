#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "defs.h"

namespace rmdb {

struct ScanCacheEntry {
    std::vector<Rid> rids;
    std::vector<std::pair<std::string, uint64_t>> epochs;
};

inline std::mutex &scan_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::string, ScanCacheEntry> &scan_cache_entries() {
    static std::unordered_map<std::string, ScanCacheEntry> entries;
    return entries;
}

inline std::list<std::string> &scan_cache_lru() {
    static std::list<std::string> lru;
    return lru;
}

inline std::unordered_map<std::string, std::list<std::string>::iterator> &scan_cache_lru_positions() {
    static std::unordered_map<std::string, std::list<std::string>::iterator> positions;
    return positions;
}

inline std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> &scan_cache_epochs() {
    static std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> epochs;
    return epochs;
}

inline size_t scan_cache_max_entries() {
    return 256;
}

inline void erase_scan_cache_entry_locked(const std::string &key) {
    auto pos_it = scan_cache_lru_positions().find(key);
    if (pos_it != scan_cache_lru_positions().end()) {
        scan_cache_lru().erase(pos_it->second);
        scan_cache_lru_positions().erase(pos_it);
    }
    scan_cache_entries().erase(key);
}

inline void touch_scan_cache_entry_locked(const std::string &key) {
    auto pos_it = scan_cache_lru_positions().find(key);
    if (pos_it == scan_cache_lru_positions().end()) {
        return;
    }
    scan_cache_lru().splice(scan_cache_lru().begin(), scan_cache_lru(), pos_it->second);
    pos_it->second = scan_cache_lru().begin();
}

inline uint64_t scan_cache_column_epoch_locked(const std::string &tab_name, const std::string &col_name) {
    return scan_cache_epochs()[tab_name][col_name];
}

inline void bump_scan_cache_columns(const std::string &tab_name, const std::vector<std::string> &col_names) {
    if (col_names.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(scan_cache_mutex());
    auto &table_epochs = scan_cache_epochs()[tab_name];
    for (const auto &col_name : col_names) {
        table_epochs[col_name]++;
    }
}

inline std::optional<std::vector<Rid>> lookup_scan_cache(const std::string &key) {
    std::lock_guard<std::mutex> guard(scan_cache_mutex());
    auto iter = scan_cache_entries().find(key);
    if (iter == scan_cache_entries().end()) {
        return std::nullopt;
    }
    for (const auto &[epoch_key, epoch] : iter->second.epochs) {
        auto sep = epoch_key.find('\x1f');
        if (sep == std::string::npos) {
            erase_scan_cache_entry_locked(key);
            return std::nullopt;
        }
        std::string tab_name = epoch_key.substr(0, sep);
        std::string col_name = epoch_key.substr(sep + 1);
        if (scan_cache_column_epoch_locked(tab_name, col_name) != epoch) {
            erase_scan_cache_entry_locked(key);
            return std::nullopt;
        }
    }
    touch_scan_cache_entry_locked(key);
    return iter->second.rids;
}

inline void store_scan_cache(const std::string &key, const std::string &tab_name,
                             const std::vector<std::string> &col_names, const std::vector<Rid> &rids) {
    std::lock_guard<std::mutex> guard(scan_cache_mutex());
    if (scan_cache_max_entries() == 0) {
        return;
    }
    ScanCacheEntry entry;
    entry.rids = rids;
    entry.epochs.reserve(col_names.size());
    for (const auto &col_name : col_names) {
        std::string epoch_key = tab_name + '\x1f' + col_name;
        entry.epochs.push_back({epoch_key, scan_cache_column_epoch_locked(tab_name, col_name)});
    }
    scan_cache_entries()[key] = std::move(entry);
    auto pos_it = scan_cache_lru_positions().find(key);
    if (pos_it != scan_cache_lru_positions().end()) {
        scan_cache_lru().erase(pos_it->second);
    }
    scan_cache_lru().push_front(key);
    scan_cache_lru_positions()[key] = scan_cache_lru().begin();
    while (scan_cache_entries().size() > scan_cache_max_entries() && !scan_cache_lru().empty()) {
        std::string evict_key = scan_cache_lru().back();
        erase_scan_cache_entry_locked(evict_key);
    }
}

}  // namespace rmdb
