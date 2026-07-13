/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unordered_set>
#include <unistd.h>
#include <atomic>

#include "errors.h"
#include "common/output_file.h"
#include "common/perf_stats.h"
#include "common/hidden_index_advisor.h"
#include "common/plan_template_cache.h"
#include "common/query_template_cache.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"

#define DEFAULT_SOCK_PORT 8765
#define MAX_CONN_LIMIT 64

static volatile sig_atomic_t should_exit = 0;

namespace {

std::atomic<uint64_t> next_client_affinity_slot{0};
std::atomic<bool> client_cleanup_failed{false};
constexpr size_t kMaxSqlCommandBytes = BUFFER_LENGTH - 1;
constexpr int kDefaultExplicitTxnIdleSeconds = 60;
constexpr int kDefaultConnectionIdleSeconds = 300;
constexpr int kDefaultSocketSendTimeoutSeconds = 30;
constexpr int kDefaultKeepAliveIdleSeconds = 60;
constexpr int kDefaultKeepAliveIntervalSeconds = 10;
constexpr int kDefaultKeepAliveProbeCount = 3;
constexpr size_t kMaxBufferPoolPages = 524288;
constexpr uint64_t kDefaultWalResetBytes = 1024ULL * 1024 * 1024;
constexpr uint64_t kMaxWalResetBytes = 64ULL * 1024 * 1024 * 1024;
constexpr int kWalResetRetrySeconds = 5;
constexpr size_t kWalPreflushBatchPages = 32768;
constexpr size_t kWalPreflushBatchFrames = 32768;
uint64_t wal_reset_bytes = 0;
uint64_t wal_preflush_bytes = 0;
std::chrono::steady_clock::time_point next_wal_reset_attempt = std::chrono::steady_clock::time_point::min();

int bounded_env_int(const char *name, int default_value, int min_value, int max_value) {
    const char *raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }
    char *end = nullptr;
    errno = 0;
    long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed < min_value || parsed > max_value) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

int server_port() {
    return bounded_env_int("RMDB_PORT", DEFAULT_SOCK_PORT, 1, 65535);
}

size_t configured_buffer_pool_pages() {
    const char *raw = std::getenv("RMDB_BUFFER_POOL_PAGES");
    if (raw == nullptr || raw[0] == '\0') {
        return BUFFER_POOL_SIZE;
    }
    for (const char *cursor = raw; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            throw InternalError("RMDB_BUFFER_POOL_PAGES must be a positive integer");
        }
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed == 0 || parsed > kMaxBufferPoolPages) {
        throw InternalError("RMDB_BUFFER_POOL_PAGES must be between 1 and " +
                            std::to_string(kMaxBufferPoolPages));
    }
    return static_cast<size_t>(parsed);
}

std::string configured_perf_stats_path() {
    const char *raw = std::getenv("RMDB_PERF_STATS_PATH");
    return raw == nullptr ? std::string() : std::string(raw);
}

uint64_t configured_wal_reset_bytes() {
    const char *raw = std::getenv("RMDB_WAL_RESET_BYTES");
    if (raw == nullptr || raw[0] == '\0') {
        return kDefaultWalResetBytes;
    }
    for (const char *cursor = raw; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            throw InternalError("RMDB_WAL_RESET_BYTES must be a non-negative integer");
        }
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed > kMaxWalResetBytes) {
        throw InternalError("RMDB_WAL_RESET_BYTES must be between 0 and " +
                            std::to_string(kMaxWalResetBytes));
    }
    return static_cast<uint64_t>(parsed);
}

uint64_t configured_wal_preflush_bytes(uint64_t reset_bytes) {
    const char *raw = std::getenv("RMDB_WAL_PREFLUSH_BYTES");
    if (raw == nullptr || raw[0] == '\0') {
        return reset_bytes == 0 ? 0 : reset_bytes - reset_bytes / 20;
    }
    for (const char *cursor = raw; *cursor != '\0'; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
            throw InternalError("RMDB_WAL_PREFLUSH_BYTES must be a non-negative integer");
        }
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed > reset_bytes) {
        throw InternalError("RMDB_WAL_PREFLUSH_BYTES must be between 0 and RMDB_WAL_RESET_BYTES");
    }
    return static_cast<uint64_t>(parsed);
}

void ConfigureAllocator() {
    if (mallopt(M_ARENA_MAX, 4) == 0) {
        throw InternalError("Failed to set the glibc arena limit");
    }
}

int explicit_txn_idle_seconds() {
    return bounded_env_int("RMDB_TXN_IDLE_TIMEOUT_SECONDS", kDefaultExplicitTxnIdleSeconds, 1, 86400);
}

int connection_idle_seconds() {
    return bounded_env_int("RMDB_CONNECTION_IDLE_TIMEOUT_SECONDS", kDefaultConnectionIdleSeconds, 1, 86400);
}

void ConfigureClientSocket(int fd) {
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));

    int keepalive_idle = kDefaultKeepAliveIdleSeconds;
    int keepalive_interval = kDefaultKeepAliveIntervalSeconds;
    int keepalive_count = kDefaultKeepAliveProbeCount;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle, sizeof(keepalive_idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count));

    timeval send_timeout{kDefaultSocketSendTimeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
}

enum class ReceiveCommandStatus { kCommand, kPeerClosed, kIdleTimeout, kTooLarge, kError, kShutdown };

bool IsAsciiCaseInsensitiveCommand(const std::string &input, const char *expected) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    size_t expected_size = std::strlen(expected);
    if (end - begin != expected_size) {
        return false;
    }
    for (size_t i = 0; i < expected_size; ++i) {
        if (std::tolower(static_cast<unsigned char>(input[begin + i])) !=
            std::tolower(static_cast<unsigned char>(expected[i]))) {
            return false;
        }
    }
    return true;
}

size_t FindLegacyCommandEnd(const std::string &pending) {
    // The pinned TPCC-Tester uses request/response sequencing but does not append NUL to requests.
    // Accept its SQL terminator without falling back to the unsafe "one recv == one command" rule.
    bool in_string = false;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i] == '\'') {
            if (in_string && i + 1 < pending.size() && pending[i + 1] == '\'') {
                ++i;
                continue;
            }
            in_string = !in_string;
        } else if (pending[i] == ';' && !in_string) {
            return i + 1;
        }
    }
    if (IsAsciiCaseInsensitiveCommand(pending, "set output_file off")) {
        return pending.size();
    }
    return std::string::npos;
}

ReceiveCommandStatus ReceiveCommand(int fd, std::string *pending, std::string *command, bool explicit_txn) {
    if (pending == nullptr || command == nullptr) {
        return ReceiveCommandStatus::kError;
    }
    const int idle_seconds = explicit_txn ? explicit_txn_idle_seconds() : connection_idle_seconds();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(idle_seconds);
    char recv_buffer[4096];

    while (true) {
        size_t delimiter = pending->find('\0');
        if (delimiter != std::string::npos) {
            if (delimiter == 0) {
                pending->erase(0, 1);
                continue;
            }
            if (delimiter > kMaxSqlCommandBytes) {
                return ReceiveCommandStatus::kTooLarge;
            }
            command->assign(pending->data(), delimiter);
            pending->erase(0, delimiter + 1);
            return ReceiveCommandStatus::kCommand;
        }
        size_t legacy_end = FindLegacyCommandEnd(*pending);
        if (legacy_end != std::string::npos) {
            if (legacy_end > kMaxSqlCommandBytes) {
                return ReceiveCommandStatus::kTooLarge;
            }
            command->assign(pending->data(), legacy_end);
            pending->erase(0, legacy_end);
            return ReceiveCommandStatus::kCommand;
        }
        if (pending->size() > kMaxSqlCommandBytes) {
            return ReceiveCommandStatus::kTooLarge;
        }
        if (should_exit) {
            return ReceiveCommandStatus::kShutdown;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return ReceiveCommandStatus::kIdleTimeout;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int poll_timeout = static_cast<int>(std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
        pollfd descriptor{fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, poll_timeout);
        if (poll_result == 0) {
            return ReceiveCommandStatus::kIdleTimeout;
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReceiveCommandStatus::kError;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return ReceiveCommandStatus::kError;
        }

        ssize_t received = recv(fd, recv_buffer, sizeof(recv_buffer), 0);
        if (received == 0) {
            return ReceiveCommandStatus::kPeerClosed;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return ReceiveCommandStatus::kError;
        }
        pending->append(recv_buffer, static_cast<size_t>(received));
    }
}

bool SendAll(int fd, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t result = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

class ActiveClientRegistry {
public:
    bool Register(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || client_fds_.size() >= MAX_CONN_LIMIT) {
            return false;
        }
        client_fds_.insert(fd);
        return true;
    }

    void CloseAndUnregister(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = client_fds_.find(fd);
        if (iter != client_fds_.end()) {
            close(fd);
            client_fds_.erase(iter);
        }
        empty_cv_.notify_all();
    }

    void BeginShutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (int fd : client_fds_) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    void WaitUntilEmpty() {
        std::unique_lock<std::mutex> lock(mutex_);
        empty_cv_.wait(lock, [&] { return client_fds_.empty(); });
    }

private:
    std::mutex mutex_;
    std::condition_variable empty_cv_;
    std::unordered_set<int> client_fds_;
    bool stopping_{false};
};

ActiveClientRegistry active_clients;

struct ClientThreadArg {
    int fd;
};

class ClientConnectionGuard {
public:
    explicit ClientConnectionGuard(int fd) : fd_(fd) {}
    ClientConnectionGuard(const ClientConnectionGuard &) = delete;
    ClientConnectionGuard &operator=(const ClientConnectionGuard &) = delete;
    ~ClientConnectionGuard() { active_clients.CloseAndUnregister(fd_); }

private:
    int fd_;
};

bool client_affinity_enabled() {
    const char *env = std::getenv("RMDB_CLIENT_AFFINITY");
    if (env == nullptr || env[0] == '\0') {
        return false;
    }
    return env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' || env[0] == 'T' ||
           env[0] == 'o' || env[0] == 'O';
}

void PinCurrentClientThread() {
    if (!client_affinity_enabled()) {
        return;
    }

    cpu_set_t allowed_set;
    CPU_ZERO(&allowed_set);
    if (sched_getaffinity(0, sizeof(allowed_set), &allowed_set) != 0) {
        return;
    }

    int allowed_count = CPU_COUNT(&allowed_set);
    if (allowed_count <= 0) {
        return;
    }

    uint64_t slot = next_client_affinity_slot.fetch_add(1, std::memory_order_relaxed);
    int target_index = static_cast<int>(slot % static_cast<uint64_t>(allowed_count));
    int target_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed_set)) {
            continue;
        }
        if (target_index == 0) {
            target_cpu = cpu;
            break;
        }
        --target_index;
    }
    if (target_cpu < 0) {
        return;
    }

    cpu_set_t target_set;
    CPU_ZERO(&target_set);
    CPU_SET(target_cpu, &target_set);
    pthread_setaffinity_np(pthread_self(), sizeof(target_set), &target_set);
}

std::string trim_copy(std::string str) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    str.erase(str.begin(), std::find_if_not(str.begin(), str.end(), is_space));
    str.erase(std::find_if_not(str.rbegin(), str.rend(), is_space).base(), str.end());
    return str;
}

std::string lowercase_copy(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return str;
}

bool server_trace_enabled() {
    const char *env = std::getenv("RMDB_TRACE_SQL");
    if (env == nullptr || env[0] == '\0') {
        return false;
    }
    std::string value = lowercase_copy(trim_copy(env));
    return value != "0" && value != "false" && value != "off" && value != "no";
}

enum class FastCommand {
    None,
    Begin,
    Commit,
    Rollback,
    Abort,
    SetOutputFileOff,
    SetIsolationSnapshot,
    SetIsolationSerializable,
};

FastCommand parse_fast_command(const char *raw_sql) {
    std::string stmt = trim_copy(raw_sql == nullptr ? "" : std::string(raw_sql));
    if (!stmt.empty() && stmt.back() == ';') {
        stmt.pop_back();
        stmt = trim_copy(stmt);
    }
    stmt = lowercase_copy(stmt);
    if (stmt == "begin") {
        return FastCommand::Begin;
    }
    if (stmt == "commit") {
        return FastCommand::Commit;
    }
    if (stmt == "rollback") {
        return FastCommand::Rollback;
    }
    if (stmt == "abort") {
        return FastCommand::Abort;
    }
    if (stmt == "set output_file off") {
        return FastCommand::SetOutputFileOff;
    }
    if (stmt.find("set transaction isolation level snapshot") != std::string::npos) {
        return FastCommand::SetIsolationSnapshot;
    }
    if (stmt.find("set transaction isolation level serializable") != std::string::npos) {
        return FastCommand::SetIsolationSerializable;
    }
    return FastCommand::None;
}

}  // namespace

std::unique_ptr<DiskManager> disk_manager;
std::unique_ptr<BufferPoolManager> buffer_pool_manager;
std::unique_ptr<RmManager> rm_manager;
std::unique_ptr<IxManager> ix_manager;
std::unique_ptr<SmManager> sm_manager;
std::unique_ptr<LockManager> lock_manager;
std::unique_ptr<TransactionManager> txn_manager;
std::unique_ptr<Planner> planner;
std::unique_ptr<Optimizer> optimizer;
std::unique_ptr<QlManager> ql_manager;
std::unique_ptr<LogManager> log_manager;
std::unique_ptr<RecoveryManager> recovery;
std::unique_ptr<Portal> portal;
std::unique_ptr<Analyze> analyze;
std::unique_ptr<RuntimePerfStats> perf_stats;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

struct WalPreflushState {
    bool started{false};
    bool complete{false};
    size_t next_frame{0};
    size_t flushed_pages{0};
    std::chrono::steady_clock::time_point started_at{};
};

WalPreflushState wal_preflush_state;

void InitializeManagers(size_t buffer_pool_pages, const std::string &perf_stats_path) {
    if (!perf_stats_path.empty()) {
        perf_stats = std::make_unique<RuntimePerfStats>(perf_stats_path, buffer_pool_pages);
    }
    disk_manager = std::make_unique<DiskManager>();
    disk_manager->set_perf_stats(perf_stats.get());
    buffer_pool_manager = std::make_unique<BufferPoolManager>(buffer_pool_pages, disk_manager.get());
    buffer_pool_manager->set_perf_stats(perf_stats.get());
    rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
    ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
    sm_manager =
        std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
    lock_manager = std::make_unique<LockManager>();
    txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
    planner = std::make_unique<Planner>(sm_manager.get());
    optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
    ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get(), nullptr);
    log_manager = std::make_unique<LogManager>(disk_manager.get());
    buffer_pool_manager->set_log_manager(log_manager.get());
    recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get());
    portal = std::make_unique<Portal>(sm_manager.get());
    analyze = std::make_unique<Analyze>(sm_manager.get());
}

void WritePerfStats() {
    if (perf_stats != nullptr && !perf_stats->Write()) {
        std::cerr << "Failed to write RMDB_PERF_STATS_PATH\n";
    }
}

void PersistRecoveredState() {
    log_manager->flush_log_to_disk();
    disk_manager->sync_log();
    for (auto &entry : sm_manager->fhs_) {
        entry.second->flush();
    }
    for (auto &entry : sm_manager->ihs_) {
        entry.second->flush();
    }
    disk_manager->sync_all_data_files();
    disk_manager->truncate_log();
    disk_manager->sync_log();
    disk_manager->remove_file_if_exists(CHECKPOINT_FILE_NAME);
    log_manager->reset_log_file_offset(0);
}

void TriggerWalResetFailpoint(const char *name) {
    const char *configured = std::getenv("RMDB_WAL_RESET_FAILPOINT");
    if (configured != nullptr && std::strcmp(configured, name) == 0) {
        std::cerr << "WAL reset failpoint: " << name << std::endl;
        _exit(86);
    }
}

bool PerformWalReset() {
    bool wal_truncated = false;
    auto pause_start = std::chrono::steady_clock::now();
    auto stage_start = pause_start;
    auto stage_elapsed_ms = [&stage_start]() {
        auto now = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(now - stage_start).count();
        stage_start = now;
        return elapsed_ms;
    };
    try {
        auto admission_guard = txn_manager->BlockNewTransactionsAndWait();
        auto checkpoint_guard = txn_manager->EnterCheckpointExecution();
        double drain_ms = stage_elapsed_ms();

        log_manager->flush_log_to_disk();
        disk_manager->sync_log();
        int64_t wal_size = disk_manager->get_file_size(LOG_FILE_NAME);
        if (wal_size < 0) {
            throw InternalError("Failed to read WAL size before reset");
        }
        double wal_sync_ms = stage_elapsed_ms();

        // DELETE is published through in-memory MVCC metadata and is only made
        // physical once no reader can need the old tuple.  With admission
        // drained there are no such readers, so materialize committed deletes
        // before making the data files durable and discarding their WAL.
        txn_manager->PhysicalizeCommittedDeletes();
        for (auto &entry : sm_manager->fhs_) {
            entry.second->flush_header();
        }
        for (auto &entry : sm_manager->ihs_) {
            entry.second->flush_header();
        }
        double metadata_ms = stage_elapsed_ms();
        size_t dirty_pages = buffer_pool_manager->flush_all_pages();
        double page_flush_ms = stage_elapsed_ms();
        TriggerWalResetFailpoint("before_data_sync");

        disk_manager->sync_all_data_files();
        double data_sync_ms = stage_elapsed_ms();
        TriggerWalResetFailpoint("after_data_sync");

        disk_manager->truncate_log();
        wal_truncated = true;
        disk_manager->sync_log();
        TriggerWalResetFailpoint("after_truncate");

        disk_manager->remove_file_if_exists(CHECKPOINT_FILE_NAME);
        log_manager->reset_log_file_offset(0);
        double log_reset_ms = stage_elapsed_ms();
        malloc_trim(0);
        double trim_ms = stage_elapsed_ms();

        auto pause_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - pause_start)
                                                  .count());
        if (perf_stats != nullptr) {
            perf_stats->RecordWalReset(pause_ns, dirty_pages, static_cast<uint64_t>(wal_size));
        }
        std::cerr << "WAL reset completed: bytes=" << wal_size << " dirty_pages=" << dirty_pages
                  << " pause_ms=" << (pause_ns / 1000000.0) << " drain_ms=" << drain_ms
                  << " wal_sync_ms=" << wal_sync_ms << " metadata_ms=" << metadata_ms
                  << " page_flush_ms=" << page_flush_ms << " data_sync_ms=" << data_sync_ms
                  << " log_reset_ms=" << log_reset_ms << " trim_ms=" << trim_ms << "\n";
        return true;
    } catch (const std::exception &error) {
        if (wal_truncated) {
            throw;
        }
        next_wal_reset_attempt =
            std::chrono::steady_clock::now() + std::chrono::seconds(kWalResetRetrySeconds);
        std::cerr << "WAL reset cancelled with log preserved: " << error.what() << "\n";
        return false;
    }
}

void ResetWalPreflushState() { wal_preflush_state = WalPreflushState{}; }

void MaybePreflushWal(uint64_t log_offset) {
    if (wal_preflush_bytes == 0 || wal_preflush_state.complete) {
        return;
    }
    if (log_offset < wal_preflush_bytes) {
        return;
    }

    if (!wal_preflush_state.started) {
        wal_preflush_state.started = true;
        wal_preflush_state.started_at = std::chrono::steady_clock::now();
    }

    bool pass_complete = false;
    wal_preflush_state.flushed_pages += buffer_pool_manager->flush_unpinned_pages_batch(
        kWalPreflushBatchPages, kWalPreflushBatchFrames, &wal_preflush_state.next_frame, &pass_complete);
    if (!pass_complete) {
        return;
    }

    wal_preflush_state.complete = true;
    auto sync_started_at = std::chrono::steady_clock::now();
    disk_manager->sync_all_data_files();
    auto sync_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - sync_started_at)
                       .count();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - wal_preflush_state.started_at)
                          .count();
    std::cerr << "WAL preflush completed: bytes=" << log_offset
              << " dirty_pages=" << wal_preflush_state.flushed_pages << " elapsed_ms=" << elapsed_ms
              << " data_sync_ms=" << sync_ms << "\n";
}

void MaybeResetWal() {
    if (wal_reset_bytes == 0 || std::chrono::steady_clock::now() < next_wal_reset_attempt) {
        return;
    }
    lsn_t log_offset = log_manager->get_log_file_offset();
    if (log_offset < 0) {
        return;
    }
    uint64_t current_log_offset = static_cast<uint64_t>(log_offset);
    if (current_log_offset < wal_reset_bytes) {
        try {
            MaybePreflushWal(current_log_offset);
        } catch (const std::exception &error) {
            ResetWalPreflushState();
            next_wal_reset_attempt =
                std::chrono::steady_clock::now() + std::chrono::seconds(kWalResetRetrySeconds);
            std::cerr << "WAL preflush cancelled with log preserved: " << error.what() << "\n";
        }
        return;
    }
    if (PerformWalReset()) {
        ResetWalPreflushState();
        next_wal_reset_attempt = std::chrono::steady_clock::time_point::min();
    }
}

void *WalResetMaintenanceLoop(void *) {
    while (!should_exit) {
        MaybeResetWal();
        poll(nullptr, 0, 250);
    }
    return nullptr;
}

void shutdown_signal_handler(int signo) {
    (void)signo;
    should_exit = true;
}

void InstallSignalHandlers() {
    struct sigaction shutdown_action {};
    shutdown_action.sa_handler = shutdown_signal_handler;
    sigemptyset(&shutdown_action.sa_mask);
    shutdown_action.sa_flags = 0;
    sigaction(SIGINT, &shutdown_action, nullptr);
    sigaction(SIGTERM, &shutdown_action, nullptr);

    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    ignore_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_pipe, nullptr);
}

void AbortDisconnectedTransaction(txn_id_t *txn_id) noexcept {
    if (txn_id == nullptr || *txn_id == INVALID_TXN_ID) {
        return;
    }
    try {
        auto statement_guard = txn_manager->EnterStatementExecution();
        Transaction *txn = txn_manager->get_transaction(*txn_id);
        if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
            txn_manager->abort(txn, log_manager.get());
        }
    } catch (const std::exception &err) {
        client_cleanup_failed.store(true, std::memory_order_release);
        std::cerr << "Failed to abort disconnected transaction " << *txn_id << ": " << err.what() << "\n";
    } catch (...) {
        client_cleanup_failed.store(true, std::memory_order_release);
        std::cerr << "Failed to abort disconnected transaction " << *txn_id << "\n";
    }
    *txn_id = INVALID_TXN_ID;
}

class SessionTransactionGuard {
public:
    explicit SessionTransactionGuard(txn_id_t *txn_id) : txn_id_(txn_id) {}
    SessionTransactionGuard(const SessionTransactionGuard &) = delete;
    SessionTransactionGuard &operator=(const SessionTransactionGuard &) = delete;
    ~SessionTransactionGuard() { AbortDisconnectedTransaction(txn_id_); }

private:
    txn_id_t *txn_id_;
};

void AbortStatementTransaction(Context *context, txn_id_t *txn_id) {
    auto statement_guard = txn_manager->EnterStatementExecution();
    Transaction *txn = context == nullptr ? nullptr : context->txn_;
    if (txn == nullptr && txn_id != nullptr && *txn_id != INVALID_TXN_ID) {
        txn = txn_manager->get_transaction(*txn_id);
    }
    if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
        txn_manager->abort(txn, log_manager.get());
    }
    if (context != nullptr) {
        context->txn_ = nullptr;
    }
    if (txn_id != nullptr) {
        *txn_id = INVALID_TXN_ID;
    }
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context, IsolationLevel session_isolation) {
    context->txn_ = *txn_id == INVALID_TXN_ID ? nullptr : txn_manager->get_transaction(*txn_id);
    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, session_isolation);
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
    }
}

void BindExistingTransaction(txn_id_t txn_id, Context *context) {
    context->txn_ = nullptr;
    if (txn_id == INVALID_TXN_ID) {
        return;
    }
    context->txn_ = txn_manager->get_transaction(txn_id);
    if (context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = nullptr;
        return;
    }
}

bool ExecuteFastCommand(FastCommand command, txn_id_t *txn_id, Context *context,
                        IsolationLevel *session_isolation) {
    if (command == FastCommand::None) {
        return false;
    }

    if (command == FastCommand::Begin) {
        BindExistingTransaction(*txn_id, context);
        if (context->txn_ == nullptr) {
            context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, *session_isolation);
        }
    }
    auto statement_guard = txn_manager->EnterStatementExecution();
    switch (command) {
        case FastCommand::SetOutputFileOff:
            rmdb::set_output_file_enabled(false);
            break;
        case FastCommand::Begin:
            context->txn_->set_txn_mode(true);
            *txn_id = context->txn_->get_transaction_id();
            break;
        case FastCommand::Commit:
            BindExistingTransaction(*txn_id, context);
            if (context->txn_ != nullptr) {
                txn_manager->commit(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
            }
            *txn_id = INVALID_TXN_ID;
            break;
        case FastCommand::Rollback:
        case FastCommand::Abort:
            BindExistingTransaction(*txn_id, context);
            if (context->txn_ != nullptr) {
                txn_manager->abort(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
            }
            *txn_id = INVALID_TXN_ID;
            break;
        case FastCommand::SetIsolationSnapshot:
        case FastCommand::SetIsolationSerializable:
            *session_isolation = command == FastCommand::SetIsolationSerializable
                                     ? IsolationLevel::SERIALIZABLE
                                     : IsolationLevel::SNAPSHOT_ISOLATION;
            BindExistingTransaction(*txn_id, context);
            break;
        case FastCommand::None:
            break;
    }
    return true;
}

void RunPlan(const std::shared_ptr<Plan> &plan, txn_id_t *txn_id, Context *context) {
    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
    portal->run(portalStmt, ql_manager.get(), txn_id, context);
    portal->drop();
    if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
        context->txn_->get_state() == TransactionState::GROWING) {
        txn_manager->commit(context->txn_, context->log_mgr_);
        context->txn_ = nullptr;
        *txn_id = INVALID_TXN_ID;
    }
}

void *client_handler(void *sock_fd) {
    std::unique_ptr<ClientThreadArg> thread_arg(static_cast<ClientThreadArg *>(sock_fd));
    int fd = thread_arg->fd;
    ClientConnectionGuard connection_guard(fd);
    PinCurrentClientThread();
    ConfigureClientSocket(fd);

    std::string pending_input;
    std::string command;
    // 需要返回给客户端的结果
    auto data_send = std::make_unique<char[]>(BUFFER_LENGTH);
    // 需要返回给客户端的结果的长度
    int offset = 0;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;
    SessionTransactionGuard transaction_guard(&txn_id);
    IsolationLevel session_isolation = IsolationLevel::SNAPSHOT_ISOLATION;
    StatementScratch statement_scratch;

    if (server_trace_enabled()) {
        std::cout << "establish client connection, sockfd: " << fd << "\n";
    }

    try {
    while (true) {
        command.clear();
        auto receive_status = ReceiveCommand(fd, &pending_input, &command, txn_id != INVALID_TXN_ID);
        if (receive_status != ReceiveCommandStatus::kCommand) {
            if (server_trace_enabled()) {
                if (receive_status == ReceiveCommandStatus::kIdleTimeout) {
                    std::cout << "Client idle timeout, sockfd: " << fd << std::endl;
                } else if (receive_status == ReceiveCommandStatus::kTooLarge) {
                    std::cout << "Client command exceeds limit, sockfd: " << fd << std::endl;
                } else {
                    std::cout << "Client connection closed, sockfd: " << fd << std::endl;
                }
            }
            if (receive_status == ReceiveCommandStatus::kTooLarge) {
                static constexpr char failure[] = "failure\n";
                SendAll(fd, failure, sizeof(failure));
            }
            break;
        }
        const char *data_recv = command.c_str();

        if (strcmp(data_recv, "exit") == 0) {
            if (server_trace_enabled()) {
                std::cout << "Client exit." << std::endl;
            }
            break;
        }
        if (strcmp(data_recv, "crash") == 0) {
            std::cout << "Server crash" << std::endl;
            exit(1);
        }

        memset(data_send.get(), '\0', BUFFER_LENGTH);
        offset = 0;
        statement_scratch.reset();

        // 开启事务，初始化系统所需的上下文信息（包括事务对象指针、锁管理器指针、日志管理器指针、存放结果的buffer、记录结果长度的变量）
        Context statement_context(lock_manager.get(), log_manager.get(), txn_manager.get(), nullptr, data_send.get(),
                                  &offset, &session_isolation, &statement_scratch);
        Context *context = &statement_context;

        bool parse_failed = false;
        try {
            FastCommand fast_command = parse_fast_command(data_recv);
            bool statement_success = false;
            if (ExecuteFastCommand(fast_command, &txn_id, context, &session_isolation)) {
                // Fast commands intentionally return an empty success response.
                statement_success = true;
            } else {
                std::shared_ptr<Query> query;
                auto template_candidate = rmdb::make_sql_template_candidate(data_recv);
                bool plan_template_candidate = template_candidate.has_value() &&
                                               rmdb::plan_template_cacheable_session(session_isolation);
                bool advisor_observes_select =
                    rmdb::hidden_index_advisor_should_observe_sql(sm_manager.get(), data_recv, session_isolation);
                bool ran_plan_template = false;
                if (plan_template_candidate && !advisor_observes_select) {
                    auto plan = rmdb::lookup_plan_template(*template_candidate, session_isolation, context);
                    if (plan != nullptr) {
                        SetTransaction(&txn_id, context, session_isolation);
                        auto statement_guard = txn_manager->EnterStatementExecution();
                        RunPlan(plan, &txn_id, context);
                        ran_plan_template = true;
                        statement_success = true;
                    }
                }
                if (!ran_plan_template && template_candidate.has_value()) {
                    query = rmdb::lookup_query_template(*template_candidate, context);
                }

                if (!ran_plan_template && query == nullptr) {
                    pthread_mutex_lock(&buffer_mutex);
                    YY_BUFFER_STATE buf = yy_scan_string(data_recv);
                    int parse_ret = yyparse();
                    std::shared_ptr<ast::TreeNode> parsed_tree = ast::parse_tree;
                    yy_delete_buffer(buf);
                    pthread_mutex_unlock(&buffer_mutex);

                    if (parse_ret == 0) {
                        if (parsed_tree != nullptr) {
                            query = analyze->do_analyze(parsed_tree);
                            if (template_candidate.has_value()) {
                                rmdb::store_query_template(*template_candidate, parsed_tree, query);
                            }
                        }
                    } else {
                        parse_failed = true;
                    }
                }

                if (!ran_plan_template && query != nullptr) {
                    bool is_checkpoint_stmt =
                        std::dynamic_pointer_cast<ast::CreateCheckpoint>(query->parse) != nullptr ||
                        query->kind == StmtKind::CreateCheckpoint;
                    bool is_txn_end_stmt =
                        std::dynamic_pointer_cast<ast::TxnCommit>(query->parse) ||
                        std::dynamic_pointer_cast<ast::TxnAbort>(query->parse) ||
                        std::dynamic_pointer_cast<ast::TxnRollback>(query->parse) ||
                        query->kind == StmtKind::TxnCommit ||
                        query->kind == StmtKind::TxnAbort ||
                        query->kind == StmtKind::TxnRollback;
                    bool is_set_isolation_stmt =
                        std::dynamic_pointer_cast<ast::SetTransactionIsolation>(query->parse) != nullptr ||
                        query->kind == StmtKind::SetTransactionIsolation;
                    std::vector<rmdb::HiddenIndexCandidate> advisor_candidates;
                    {
                        std::shared_lock<std::shared_mutex> statement_guard;
                        std::unique_lock<std::shared_mutex> checkpoint_guard;
                        if (is_checkpoint_stmt) {
                            checkpoint_guard = txn_manager->EnterCheckpointExecution();
                        } else {
                            if (is_txn_end_stmt) {
                                BindExistingTransaction(txn_id, context);
                            } else if (!is_set_isolation_stmt) {
                                SetTransaction(&txn_id, context, session_isolation);
                            }
                            statement_guard = txn_manager->EnterStatementExecution();
                        }
                        advisor_candidates = rmdb::hidden_index_advisor_collect(sm_manager.get(), query, context);
                        // 优化器
                        std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
                        if (plan_template_candidate && !advisor_observes_select) {
                            rmdb::store_plan_template(*template_candidate, query, plan, session_isolation);
                        }
                        RunPlan(plan, &txn_id, context);
                        rmdb::hidden_index_advisor_record(advisor_candidates);
                        statement_success = true;
                    }
                }
            }
            if (statement_success && txn_id == INVALID_TXN_ID &&
                rmdb::plan_template_cacheable_session(session_isolation)) {
                rmdb::hidden_index_advisor_try_build_one(sm_manager.get(), txn_manager.get(), context);
            }
        } catch (TransactionAbortException &e) {
            // 事务需要回滚，需要把abort信息返回给客户端并写入output.txt文件中
            std::string str = "abort\n";
            memcpy(data_send.get(), str.c_str(), str.length());
            data_send[str.length()] = '\0';
            offset = str.length();

            // 回滚事务
            AbortStatementTransaction(context, &txn_id);
            std::cout << e.GetInfo() << std::endl;
            rmdb::append_output_file(str);
        } catch (RMDBError &e) {
            std::cerr << e.what() << std::endl;

            bool explicit_txn_error = context->txn_ != nullptr && context->txn_->get_txn_mode();
            AbortStatementTransaction(context, &txn_id);

            // Statement-level errors must not leave partial writes behind.  For explicit transactions,
            // report abort so clients retry instead of committing a transaction that was rolled back here.
            std::string str = explicit_txn_error ? "abort\n" : "failure\n";
            memcpy(data_send.get(), str.c_str(), str.length());
            data_send[str.length()] = '\0';
            offset = str.length();

            rmdb::append_output_file(str);
        }
        if (parse_failed) {
            std::string str = "failure\n";
            memcpy(data_send.get(), str.c_str(), str.length());
            data_send[str.length()] = '\0';
            offset = str.length();
            rmdb::append_output_file(str);
        }
        // future TODO: 格式化 sql_handler.result, 传给客户端
        // send result with fixed format, use protobuf in the future
        if (offset < 0 || offset >= BUFFER_LENGTH ||
            !SendAll(fd, data_send.get(), static_cast<size_t>(offset) + 1)) {
            break;
        }
    }
    } catch (const std::exception &err) {
        std::cerr << "Client handler error on sockfd " << fd << ": " << err.what() << "\n";
    } catch (...) {
        std::cerr << "Unknown client handler error on sockfd " << fd << "\n";
    }

    // Clear
    if (server_trace_enabled()) {
        std::cout << "Terminating current client_connection..." << std::endl;
    }
    return nullptr;
}

void start_server() {
    int sockfd_server;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    if (sockfd_server == -1) {
        throw UnixError();
    }
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(static_cast<uint16_t>(server_port()));
    if (bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in)) == -1) {
        std::cout << "Bind error!" << std::endl;
        close(sockfd_server);
        throw UnixError();
    }

    if (listen(sockfd_server, MAX_CONN_LIMIT) == -1) {
        std::cout << "Listen error!" << std::endl;
        close(sockfd_server);
        throw UnixError();
    }

    pthread_attr_t thread_attr;
    if (pthread_attr_init(&thread_attr) != 0) {
        close(sockfd_server);
        throw InternalError("Failed to initialize detached client threads");
    }
    if (pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&thread_attr);
        close(sockfd_server);
        throw InternalError("Failed to initialize detached client threads");
    }

    pthread_t wal_reset_thread{};
    bool wal_reset_thread_started = false;
    if (wal_reset_bytes != 0) {
        if (pthread_create(&wal_reset_thread, nullptr, &WalResetMaintenanceLoop, nullptr) != 0) {
            pthread_attr_destroy(&thread_attr);
            close(sockfd_server);
            throw InternalError("Failed to start WAL reset maintenance thread");
        }
        wal_reset_thread_started = true;
    }

    while (!should_exit) {
        if (server_trace_enabled()) {
            std::cout << "Waiting for new connection..." << std::endl;
        }
        pollfd listener{sockfd_server, POLLIN, 0};
        int poll_result = poll(&listener, 1, 250);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Server poll error: " << strerror(errno) << "\n";
            break;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((listener.revents & POLLIN) == 0) {
            if ((listener.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
            continue;
        }

        struct sockaddr_in s_addr_client {};
        socklen_t client_length = sizeof(s_addr_client);

        // Block here. Until server accepts a new connection.
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), &client_length);
        if (sockfd == -1) {
            if (errno != EINTR && !should_exit) {
                std::cerr << "Accept error: " << strerror(errno) << "\n";
            }
            continue;  // ignore current socket ,continue while loop.
        }

        auto *thread_arg = new (std::nothrow) ClientThreadArg{sockfd};
        if (thread_arg == nullptr || !active_clients.Register(sockfd)) {
            delete thread_arg;
            close(sockfd);
            continue;
        }

        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        pthread_t thread_id;
        if (pthread_create(&thread_id, &thread_attr, &client_handler, thread_arg) != 0) {
            std::cerr << "Create thread fail!" << std::endl;
            delete thread_arg;
            active_clients.CloseAndUnregister(sockfd);
            continue;
        }
    }
    pthread_attr_destroy(&thread_attr);

    // Clear
    close(sockfd_server);
    std::cout << "Try to close all client connections.\n";
    active_clients.BeginShutdown();
    active_clients.WaitUntilEmpty();
    if (wal_reset_thread_started) {
        pthread_join(wal_reset_thread, nullptr);
    }
    if (client_cleanup_failed.load(std::memory_order_acquire)) {
        log_manager->flush_log_to_disk();
        throw InternalError("Client transaction cleanup failed; refusing clean database shutdown");
    }
    if (txn_manager->ActiveTransactionCount() != 0) {
        log_manager->flush_log_to_disk();
        throw InternalError("Active transactions remain during database shutdown");
    }
    txn_manager->PhysicalizeCommittedDeletes();
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    InstallSignalHandlers();
    try {
        ConfigureAllocator();
        wal_reset_bytes = configured_wal_reset_bytes();
        wal_preflush_bytes = configured_wal_preflush_bytes(wal_reset_bytes);
        InitializeManagers(configured_buffer_pool_pages(), configured_perf_stats_path());
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);
        int64_t log_file_size = disk_manager->get_file_size(LOG_FILE_NAME);
        log_manager->reset_log_file_offset(log_file_size > 0 ? log_file_size : 0);

        // recovery database
        recovery->analyze();
        recovery->redo();
        recovery->undo();
        PersistRecoveredState();
        
        // 开启服务端，开始接受客户端连接
        start_server();
        WritePerfStats();
    } catch (RMDBError &e) {
        WritePerfStats();
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (const std::exception &e) {
        WritePerfStats();
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return 0;
}
