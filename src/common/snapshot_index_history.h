#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "defs.h"
#include "system/sm_meta.h"

namespace rmdb {

struct SnapshotIndexHistoryEntry {
    timestamp_t retire_ts{INVALID_TS};
    Rid rid{};
};

inline std::mutex &snapshot_index_history_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::string, std::vector<SnapshotIndexHistoryEntry>> &snapshot_index_history_entries() {
    static std::unordered_map<std::string, std::vector<SnapshotIndexHistoryEntry>> entries;
    return entries;
}

inline std::string snapshot_index_history_key(const std::string &tab_name,
                                              const std::vector<std::string> &col_names) {
    std::string key = tab_name;
    for (const auto &col_name : col_names) {
        key.push_back('\x1f');
        key += col_name;
    }
    return key;
}

inline std::string snapshot_index_history_key(const std::string &tab_name, const IndexMeta &index) {
    std::vector<std::string> col_names;
    col_names.reserve(index.cols.size());
    for (const auto &col : index.cols) {
        col_names.push_back(col.name);
    }
    return snapshot_index_history_key(tab_name, col_names);
}

inline void record_snapshot_index_retirement(const std::string &tab_name, const IndexMeta &index,
                                             const Rid &rid, timestamp_t retire_ts) {
    if (retire_ts == INVALID_TS) {
        return;
    }
    std::lock_guard<std::mutex> guard(snapshot_index_history_mutex());
    snapshot_index_history_entries()[snapshot_index_history_key(tab_name, index)].push_back({retire_ts, rid});
}

inline std::vector<Rid> lookup_snapshot_index_history(const std::string &tab_name,
                                                      const std::vector<std::string> &col_names,
                                                      timestamp_t read_ts) {
    std::vector<Rid> rids;
    std::lock_guard<std::mutex> guard(snapshot_index_history_mutex());
    auto iter = snapshot_index_history_entries().find(snapshot_index_history_key(tab_name, col_names));
    if (iter == snapshot_index_history_entries().end()) {
        return rids;
    }
    for (const auto &entry : iter->second) {
        if (entry.retire_ts > read_ts) {
            rids.push_back(entry.rid);
        }
    }
    return rids;
}

inline void purge_snapshot_index_history(timestamp_t watermark) {
    std::lock_guard<std::mutex> guard(snapshot_index_history_mutex());
    auto &all_entries = snapshot_index_history_entries();
    for (auto iter = all_entries.begin(); iter != all_entries.end();) {
        auto &entries = iter->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
                          return entry.retire_ts <= watermark;
                      }),
                      entries.end());
        if (entries.empty()) {
            iter = all_entries.erase(iter);
        } else {
            ++iter;
        }
    }
}

}  // namespace rmdb
