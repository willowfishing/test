#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>

namespace rmdb {

inline std::atomic<bool> &output_file_enabled_flag() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline std::mutex &output_file_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline void set_output_file_enabled(bool enabled) {
    output_file_enabled_flag().store(enabled, std::memory_order_relaxed);
}

inline bool is_output_file_enabled() {
    return output_file_enabled_flag().load(std::memory_order_relaxed);
}

inline void append_output_file(const std::string &text) {
    if (!is_output_file_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> guard(output_file_mutex());
    std::ofstream outfile("output.txt", std::ios::out | std::ios::app);
    outfile << text;
}

}  // namespace rmdb
