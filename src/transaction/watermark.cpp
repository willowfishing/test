/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction/watermark.h"


auto Watermark::AddTxn(timestamp_t read_ts) -> void {
    current_reads_[read_ts]++;
    // Recalculate watermark
    if (!current_reads_.empty()) {
        watermark_ = std::min(current_reads_.begin()->first, commit_ts_);
    } else {
        watermark_ = commit_ts_;
    }
}

auto Watermark::RemoveTxn(timestamp_t read_ts) -> void {
    auto it = current_reads_.find(read_ts);
    if (it != current_reads_.end()) {
        it->second--;
        if (it->second <= 0) {
            current_reads_.erase(it);
        }
    }
    // Recalculate watermark
    if (!current_reads_.empty()) {
        watermark_ = std::min(current_reads_.begin()->first, commit_ts_);
    } else {
        watermark_ = commit_ts_;
    }
}

auto Watermark::UpdateCommitTs(timestamp_t commit_ts) -> void {
    if (commit_ts > commit_ts_) {
        commit_ts_ = commit_ts;
    }
    // Recalculate watermark
    if (!current_reads_.empty()) {
        watermark_ = std::min(current_reads_.begin()->first, commit_ts_);
    } else {
        watermark_ = commit_ts_;
    }
}

auto Watermark::GetWatermark() -> timestamp_t {
    return watermark_;
}