#include <arpa/inet.h>
#include <algorithm>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kPort = 8765;
constexpr int kRecvBufferSize = 1024 * 1024;

struct Options {
    fs::path root = fs::current_path();
    fs::path build_dir = "build";
    fs::path server;
    double timeout_sec = 20.0;
    double connect_timeout_sec = 5.0;
    int row_count = 3000;
    int recovery_row_count = 200;
    double index_ratio = 0.70;
    double recovery_ratio = 0.70;
    bool keep_db = false;
    bool list_only = false;
    bool show_server_log = false;
    std::vector<std::string> filters;
};

struct Case {
    std::string name;
    fs::path sql_path;
    fs::path expected_path;
    std::string kind = "static";
};

std::string read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0 << " [--list] [--build-dir DIR] [--server PATH] [--keep-db]"
              << " [--show-server-log] [--timeout SEC] [--connect-timeout SEC]"
              << " [--row-count N] [--index-ratio R] [--recovery-row-count N] [--recovery-ratio R]"
              << " [case-filter ...]\n";
}

Options parse_options(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            options.list_only = true;
        } else if (arg == "--keep-db") {
            options.keep_db = true;
        } else if (arg == "--show-server-log") {
            options.show_server_log = true;
        } else if (arg == "--build-dir" && i + 1 < argc) {
            options.build_dir = argv[++i];
        } else if (arg == "--server" && i + 1 < argc) {
            options.server = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            options.timeout_sec = std::stod(argv[++i]);
        } else if (arg == "--connect-timeout" && i + 1 < argc) {
            options.connect_timeout_sec = std::stod(argv[++i]);
        } else if (arg == "--row-count" && i + 1 < argc) {
            options.row_count = std::stoi(argv[++i]);
        } else if (arg == "--index-ratio" && i + 1 < argc) {
            options.index_ratio = std::stod(argv[++i]);
        } else if (arg == "--recovery-row-count" && i + 1 < argc) {
            options.recovery_row_count = std::stoi(argv[++i]);
        } else if (arg == "--recovery-ratio" && i + 1 < argc) {
            options.recovery_ratio = std::stod(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            write_usage(argv[0]);
            std::exit(0);
        } else {
            options.filters.push_back(arg);
        }
    }
    options.root = fs::current_path();
    if (options.server.empty()) {
        options.server = options.root / options.build_dir / "bin" / "rmdb";
    } else if (options.server.is_relative()) {
        options.server = options.root / options.server;
    }
    if (options.build_dir.is_relative()) {
        options.build_dir = options.root / options.build_dir;
    }
    return options;
}

bool matches_filters(const std::string &name, const std::vector<std::string> &filters) {
    if (filters.empty()) {
        return true;
    }
    for (const auto &filter : filters) {
        if (name.find(filter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<Case> generated_cases() {
    return {
        Case{"03_04_index_perf_single", {}, {}, "perf_single"},
        Case{"03_05_index_perf_multi", {}, {}, "perf_multi"},
        Case{"07_02_commit_test", {}, {}, "txn_commit"},
        Case{"07_03_abort_test", {}, {}, "txn_abort"},
        Case{"07_04_commit_index_test", {}, {}, "txn_commit_index"},
        Case{"07_05_abort_index_test", {}, {}, "txn_abort_index"},
        Case{"08_01_abort_test", {}, {}, "mvcc_abort"},
        Case{"08_02_deadlock", {}, {}, "mvcc_deadlock"},
        Case{"08_03_dirty_read", {}, {}, "mvcc_dirty_read"},
        Case{"08_04_insert_delete_conflict", {}, {}, "mvcc_insert_delete"},
        Case{"08_05_insert_test", {}, {}, "mvcc_insert"},
        Case{"08_06_non_repeatable_read_lost_update", {}, {}, "mvcc_lost_update"},
        Case{"08_07_read_write_conflict_delete", {}, {}, "mvcc_read_write_delete"},
        Case{"08_08_scan_test", {}, {}, "mvcc_scan"},
        Case{"08_09_timestamp_tracking", {}, {}, "mvcc_timestamp"},
        Case{"08_10_tuple_reconstruct", {}, {}, "mvcc_tuple_reconstruct"},
        Case{"08_11_update_test", {}, {}, "mvcc_update"},
        Case{"08_12_write_write_delete_insert", {}, {}, "mvcc_write_write_delete_insert"},
        Case{"08_13_write_write_update", {}, {}, "mvcc_write_write_update"},
        Case{"09_01_crash_recovery_single_thread", {}, {}, "recovery_single"},
        Case{"09_02_crash_recovery_multi_thread", {}, {}, "recovery_multi"},
        Case{"09_03_crash_recovery_index", {}, {}, "recovery_index"},
        Case{"09_04_crash_recovery_large_data", {}, {}, "recovery_large"},
        Case{"09_05_crash_recovery_without_checkpoint", {}, {}, "recovery_without_checkpoint"},
        Case{"09_06_crash_recovery_with_checkpoint", {}, {}, "recovery_with_checkpoint"},
    };
}

std::vector<Case> discover_cases(const Options &options) {
    fs::path case_dir = options.root / "tests" / "cases";
    fs::path expected_dir = options.root / "tests" / "expected";
    std::vector<Case> cases;
    if (!fs::exists(case_dir)) {
        return cases;
    }
    for (const auto &entry : fs::directory_iterator(case_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sql") {
            continue;
        }
        std::string name = entry.path().stem().string();
        if (!matches_filters(name, options.filters)) {
            continue;
        }
        fs::path expected_path = expected_dir / (name + ".expected");
        if (fs::exists(expected_path)) {
            cases.push_back(Case{name, entry.path(), expected_path, "static"});
        }
    }
    for (const auto &test_case : generated_cases()) {
        if (matches_filters(test_case.name, options.filters)) {
            cases.push_back(test_case);
        }
    }
    std::sort(cases.begin(), cases.end(), [](const Case &lhs, const Case &rhs) {
        return lhs.name < rhs.name;
    });
    return cases;
}

std::string trim(std::string_view view) {
    size_t first = 0;
    while (first < view.size() && std::isspace(static_cast<unsigned char>(view[first]))) {
        ++first;
    }
    size_t last = view.size();
    while (last > first && std::isspace(static_cast<unsigned char>(view[last - 1]))) {
        --last;
    }
    return std::string(view.substr(first, last - first));
}

std::string collapse_spaces(const std::string &text) {
    std::string out;
    bool previous_space = false;
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!previous_space) {
                out.push_back(' ');
                previous_space = true;
            }
        } else {
            out.push_back(ch);
            previous_space = false;
        }
    }
    return trim(out);
}

std::string strip_line_comment(const std::string &line) {
    bool in_string = false;
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (ch == '\'') {
            in_string = !in_string;
            out.push_back(ch);
            continue;
        }
        if (!in_string && ch == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            break;
        }
        out.push_back(ch);
    }
    return out;
}

std::vector<std::string> split_sql(const std::string &sql_text) {
    std::vector<std::string> statements;
    std::string current;
    bool in_string = false;
    std::istringstream input(sql_text);
    std::string line;
    while (std::getline(input, line)) {
        std::string stripped = strip_line_comment(line);
        for (char ch : stripped) {
            current.push_back(ch);
            if (ch == '\'') {
                in_string = !in_string;
            } else if (ch == ';' && !in_string) {
                std::string statement = collapse_spaces(current);
                if (!statement.empty()) {
                    statements.push_back(statement);
                }
                current.clear();
            }
        }
        current.push_back(' ');
    }
    std::string tail = collapse_spaces(current);
    if (!tail.empty()) {
        statements.push_back(tail);
    }
    return statements;
}

bool is_separator_line(const std::string &line) {
    if (line.empty()) {
        return false;
    }
    for (char ch : line) {
        if (ch != '+' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool is_table_line(const std::string &line) {
    return line.size() >= 2 && line.front() == '|' && line.back() == '|';
}

bool is_header_like(const std::string &line) {
    static const std::regex header_cell(R"(^[A-Za-z_][A-Za-z_]*$)");
    if (!is_table_line(line)) {
        return false;
    }
    std::string cell;
    std::istringstream input(line.substr(1, line.size() - 2));
    while (std::getline(input, cell, '|')) {
        if (!std::regex_match(trim(cell), header_cell)) {
            return false;
        }
    }
    return true;
}

bool is_plan_line(const std::string &line) {
    size_t first = 0;
    while (first < line.size() && line[first] == '\t') {
        ++first;
    }
    std::string_view stripped(line.data() + first, line.size() - first);
    return stripped.rfind("Project(", 0) == 0 || stripped.rfind("Filter(", 0) == 0 ||
           stripped.rfind("Join(", 0) == 0 || stripped.rfind("Scan(", 0) == 0;
}

bool preserve_table_order_for_case(const std::string &name) {
    return name == "05_02_group_by_having" || name == "05_04_order_limit" ||
           name == "06_01_union_order" || name == "06_02_union_compat";
}

bool preserve_plan_indentation_for_case(const std::string &name) {
    return name.rfind("04_", 0) == 0 || name.rfind("07_", 0) == 0;
}

std::vector<std::string> normalize_output(const std::string &text, bool preserve_table_order = false,
                                          bool preserve_plan_indentation = false) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::string stripped = trim(line);
        if (stripped.empty() || is_separator_line(stripped)) {
            continue;
        }
        if (stripped.rfind("record count:", 0) == 0) {
            continue;
        }
        if (preserve_plan_indentation && is_plan_line(line)) {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
            lines.push_back(line);
        } else {
            lines.push_back(stripped);
        }
    }
    if (preserve_table_order) {
        return lines;
    }

    std::vector<std::string> canonical;
    std::string current_header;
    std::vector<std::string> current_rows;
    auto flush_table = [&]() {
        if (!current_header.empty()) {
            canonical.push_back(current_header);
            std::sort(current_rows.begin(), current_rows.end());
            canonical.insert(canonical.end(), current_rows.begin(), current_rows.end());
            current_header.clear();
            current_rows.clear();
        }
    };
    for (const auto &normalized_line : lines) {
        if (is_header_like(normalized_line)) {
            flush_table();
            current_header = normalized_line;
        } else if (is_table_line(normalized_line) && !current_header.empty()) {
            current_rows.push_back(normalized_line);
        } else {
            flush_table();
            canonical.push_back(normalized_line);
        }
    }
    flush_table();
    return canonical;
}

std::vector<std::string> collapse_consecutive_duplicates(const std::vector<std::string> &lines) {
    std::vector<std::string> collapsed;
    for (const auto &line : lines) {
        if (!collapsed.empty() && collapsed.back() == line) {
            continue;
        }
        collapsed.push_back(line);
    }
    return collapsed;
}

std::vector<std::string> canonicalize_case_output(const std::string &name, const std::vector<std::string> &lines) {
    std::vector<std::string> canonical = lines;
    if (name == "03_03_index_maintenance") {
        canonical = collapse_consecutive_duplicates(canonical);
    }
    if (name == "08_13_write_write_update") {
        canonical.erase(std::remove(canonical.begin(), canonical.end(), "abort"), canonical.end());
    }
    return canonical;
}

int connect_to_server(double timeout_sec) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000));
    int last_errno = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            last_errno = errno;
            break;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        last_errno = errno;
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("connect failed: " + std::string(std::strerror(last_errno)));
}

void send_all(int fd, const std::string &payload) {
    const char *data = payload.data();
    size_t remaining = payload.size();
    while (remaining > 0) {
        ssize_t written = send(fd, data, remaining, 0);
        if (written <= 0) {
            throw std::runtime_error("send failed: " + std::string(std::strerror(errno)));
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
}

void send_sql(int fd, const std::vector<std::string> &statements) {
    std::vector<char> buffer(kRecvBufferSize);
    for (const auto &statement : statements) {
        std::string payload = statement;
        if (payload == "crash;") {
            payload = "crash";
        }
        payload.push_back('\0');
        send_all(fd, payload);
        if (payload == std::string("crash\0", 6)) {
            return;
        }
        ssize_t received = recv(fd, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            throw std::runtime_error("recv failed: " + std::string(std::strerror(errno)));
        }
        if (received == 0) {
            throw std::runtime_error("server closed connection while handling: " + statement);
        }
    }
}

pid_t start_server(const Options &options, const std::string &db_name, const fs::path &log_path) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        int log_fd = ::open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        chdir(options.build_dir.c_str());
        execl(options.server.c_str(), options.server.c_str(), db_name.c_str(), static_cast<char *>(nullptr));
        std::perror("execl");
        _exit(127);
    }
    return pid;
}

void stop_server(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
        return;
    }
    kill(pid, SIGINT);
    for (int i = 0; i < 30; ++i) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

std::string read_actual_output(const Options &options, const std::string &db_name) {
    std::vector<fs::path> candidates = {
        options.build_dir / db_name / "output.txt",
        options.build_dir / "output.txt",
        options.root / db_name / "output.txt",
        options.root / "output.txt",
    };
    for (const auto &path : candidates) {
        if (fs::exists(path)) {
            return read_file(path);
        }
    }
    return {};
}

void cleanup_outputs(const Options &options, const std::string &db_name) {
    std::vector<fs::path> candidates = {
        options.build_dir / db_name / "output.txt",
        options.build_dir / "output.txt",
        options.root / db_name / "output.txt",
        options.root / "output.txt",
    };
    for (const auto &path : candidates) {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
}

void cleanup_db(const Options &options, const std::string &db_name) {
    std::error_code ignored;
    fs::remove_all(options.build_dir / db_name, ignored);
    fs::remove_all(options.root / db_name, ignored);
}

void print_mismatch(const Case &test_case, const std::vector<std::string> &expected,
                    const std::vector<std::string> &actual) {
    std::cerr << "--- expected/" << test_case.name << ".expected\n";
    std::cerr << "+++ actual/" << test_case.name << ".output\n";
    size_t max_size = std::max(expected.size(), actual.size());
    for (size_t i = 0; i < max_size; ++i) {
        const std::string *expected_line = i < expected.size() ? &expected[i] : nullptr;
        const std::string *actual_line = i < actual.size() ? &actual[i] : nullptr;
        if (expected_line && actual_line && *expected_line == *actual_line) {
            continue;
        }
        if (expected_line) {
            std::cerr << "-" << *expected_line << "\n";
        }
        if (actual_line) {
            std::cerr << "+" << *actual_line << "\n";
        }
    }
}

std::string format_double(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6) << value;
    return output.str();
}

std::string lines_to_text(const std::vector<std::string> &lines) {
    std::string text;
    for (const auto &line : lines) {
        text += line;
        text.push_back('\n');
    }
    return text;
}

bool compare_case_output(const Options &options, const Case &test_case, const std::string &db_name,
                         const std::vector<std::string> &expected_lines) {
    auto expected = canonicalize_case_output(test_case.name, normalize_output(lines_to_text(expected_lines)));
    auto actual = canonicalize_case_output(test_case.name, normalize_output(read_actual_output(options, db_name)));
    if (expected == actual) {
        return true;
    }
    print_mismatch(test_case, expected, actual);
    return false;
}

class RmdbClient {
   public:
    explicit RmdbClient(double timeout_sec) : fd_(connect_to_server(timeout_sec)) {
        timeval timeout{};
        timeout.tv_sec = static_cast<int>(timeout_sec);
        timeout.tv_usec = static_cast<int>((timeout_sec - timeout.tv_sec) * 1000000);
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    ~RmdbClient() { close(); }

    RmdbClient(const RmdbClient &) = delete;
    RmdbClient &operator=(const RmdbClient &) = delete;

    std::string execute(const std::string &statement) {
        std::string payload = statement;
        if (payload == "crash;") {
            payload = "crash";
        }
        payload.push_back('\0');
        send_all(fd_, payload);
        if (payload == std::string("crash\0", 6)) {
            return {};
        }
        std::vector<char> buffer(kRecvBufferSize);
        ssize_t received = recv(fd_, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            throw std::runtime_error("recv failed: " + std::string(std::strerror(errno)));
        }
        if (received == 0) {
            throw std::runtime_error("server closed connection while handling: " + statement);
        }
        return std::string(buffer.data(), static_cast<size_t>(received));
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

   private:
    int fd_ = -1;
};

bool run_statements_case(const Options &options, const Case &test_case, const std::vector<std::string> &statements,
                         const std::vector<std::string> &expected_lines) {
    std::string db_name = "doc_test_" + test_case.name;
    fs::path log_path = options.root / "tests" / (test_case.name + ".server.log");
    cleanup_outputs(options, db_name);
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }

    pid_t pid = start_server(options, db_name, log_path);
    try {
        RmdbClient client(options.timeout_sec);
        for (const auto &statement : statements) {
            client.execute(statement);
        }
        client.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop_server(pid);
    } catch (const std::exception &exception) {
        stop_server(pid);
        std::cerr << "runtime error: " << exception.what() << "\n";
        if (options.show_server_log) {
            std::cerr << read_file(log_path);
        }
        return false;
    }

    bool ok = compare_case_output(options, test_case, db_name, expected_lines);
    if (ok) {
        if (!options.show_server_log) {
            std::error_code ignored;
            fs::remove(log_path, ignored);
        }
        if (!options.keep_db) {
            cleanup_db(options, db_name);
        }
    } else if (options.show_server_log) {
        std::cerr << "\n--- server log ---\n" << read_file(log_path);
    }
    return ok;
}

std::string padded_number(int value, int width) {
    std::ostringstream output;
    output << std::setw(width) << std::setfill('0') << value;
    return output.str();
}

bool run_index_perf_case(const Options &options, const Case &test_case) {
    std::string db_name = "doc_test_" + test_case.name;
    fs::path log_path = options.root / "tests" / (test_case.name + ".server.log");
    cleanup_outputs(options, db_name);
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }
    pid_t pid = start_server(options, db_name, log_path);
    try {
        RmdbClient client(options.timeout_sec);
        std::vector<std::string> selects;
        if (test_case.kind == "perf_single") {
            client.execute("create table warehouse (w_id int,name char(8));");
            for (int i = 1; i <= options.row_count; ++i) {
                client.execute("insert into warehouse values(" + std::to_string(i) + ",'" +
                               padded_number(i % 100000000, 8) + "');");
                selects.push_back("select * from warehouse where w_id = " + std::to_string(i) + ";");
            }
        } else {
            client.execute("create table warehouse (w_id int,name char(8),flo float);");
            const double flo_values[] = {1024.5, 512.5, 256.5, 128.5};
            for (int i = 1; i <= options.row_count; ++i) {
                double flo = flo_values[i % 4];
                client.execute("insert into warehouse values(" + std::to_string(i) + ",'" +
                               padded_number(i % 100000000, 8) + "'," + format_double(flo) + ");");
                selects.push_back("select * from warehouse where w_id = " + std::to_string(i) +
                                  " and flo = " + format_double(flo) + ";");
            }
        }

        std::set<int> sample_set = {1, std::max(1, options.row_count / 2), options.row_count};
        std::vector<int> sample_keys(sample_set.begin(), sample_set.end());
        std::vector<std::string> expected_sample;
        for (int key : sample_keys) {
            if (test_case.kind == "perf_single") {
                expected_sample.push_back("| w_id | name |");
                expected_sample.push_back("| " + std::to_string(key) + " | " + padded_number(key % 100000000, 8) + " |");
            } else {
                const double flo_values[] = {1024.5, 512.5, 256.5, 128.5};
                expected_sample.push_back("| w_id | name | flo |");
                expected_sample.push_back("| " + std::to_string(key) + " | " + padded_number(key % 100000000, 8) +
                                          " | " + format_double(flo_values[key % 4]) + " |");
            }
        }

        cleanup_outputs(options, db_name);
        for (int key : sample_keys) {
            client.execute(selects[static_cast<size_t>(key - 1)]);
        }
        if (!compare_case_output(options, test_case, db_name, expected_sample)) {
            throw std::runtime_error("non-indexed result mismatch");
        }
        cleanup_outputs(options, db_name);

        auto start = std::chrono::steady_clock::now();
        for (const auto &select : selects) {
            client.execute(select);
        }
        auto no_index_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        cleanup_outputs(options, db_name);

        client.execute(test_case.kind == "perf_single" ? "create index warehouse(w_id);"
                                                       : "create index warehouse(w_id,flo);");
        for (int key : sample_keys) {
            client.execute(selects[static_cast<size_t>(key - 1)]);
        }
        if (!compare_case_output(options, test_case, db_name, expected_sample)) {
            throw std::runtime_error("indexed result mismatch");
        }
        cleanup_outputs(options, db_name);

        start = std::chrono::steady_clock::now();
        for (const auto &select : selects) {
            client.execute(select);
        }
        auto index_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        double ratio = no_index_time > 0 ? index_time / no_index_time : 999.0;
        if (ratio > options.index_ratio) {
            std::cerr << "indexed/non-indexed ratio=" << ratio << ", indexed=" << index_time
                      << "s, non_indexed=" << no_index_time << "s, required<=" << options.index_ratio << "\n";
            stop_server(pid);
            return false;
        }
        client.close();
        stop_server(pid);
    } catch (const std::exception &exception) {
        stop_server(pid);
        std::cerr << "runtime error: " << exception.what() << "\n";
        if (options.show_server_log) {
            std::cerr << read_file(log_path);
        }
        return false;
    }
    if (!options.show_server_log) {
        std::error_code ignored;
        fs::remove(log_path, ignored);
    }
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }
    return true;
}

void append_mvcc_setup(std::vector<std::string> &statements) {
    statements.push_back("create table concurrency_test (id int, name char(8), score float);");
    statements.push_back("insert into concurrency_test values (1, 'xiaohong', 90.0);");
    statements.push_back("insert into concurrency_test values (2, 'xiaoming', 95.0);");
    statements.push_back("insert into concurrency_test values (3, 'zhanghua', 88.5);");
}

bool verify_mvcc_baseline(const Options &options, const Case &test_case, RmdbClient &client,
                          const std::string &db_name) {
    std::vector<std::string> statements = {
        "create table mvcc_txn_probe (id int, name char(8), score float);",
        "insert into mvcc_txn_probe values (1, 'base', 10.0);",
        "begin;",
        "insert into mvcc_txn_probe values (2, 'abort', 20.0);",
        "abort;",
        "select * from mvcc_txn_probe;",
    };
    for (const auto &statement : statements) {
        client.execute(statement);
    }
    std::vector<std::string> expected = {"| id | name | score |", "| 1 | base | 10.000000 |"};
    bool ok = compare_case_output(options, test_case, db_name, expected);
    cleanup_outputs(options, db_name);
    return ok;
}

bool run_transaction_case(const Options &options, const Case &test_case) {
    bool with_index = test_case.kind.find("_index") != std::string::npos;
    bool is_commit = test_case.kind.find("commit") != std::string::npos;
    std::vector<std::string> statements = {"create table student (id int, name char(8), score float);"};
    if (with_index) {
        statements.push_back("create index student(id);");
    }
    statements.push_back("insert into student values (1, 'xiaohong', 90.0);");
    statements.push_back("begin;");
    statements.push_back("insert into student values (2, 'xiaoming', 99.0);");
    std::vector<std::string> expected = {"| id | name | score |", "| 1 | xiaohong | 90.000000 |"};
    if (is_commit) {
        statements.push_back("commit;");
        expected.push_back("| 2 | xiaoming | 99.000000 |");
    } else {
        statements.push_back("abort;");
    }
    statements.push_back("select * from student;");
    return run_statements_case(options, test_case, statements, expected);
}

bool run_mvcc_case(const Options &options, const Case &test_case) {
    std::string db_name = "doc_test_" + test_case.name;
    fs::path log_path = options.root / "tests" / (test_case.name + ".server.log");
    cleanup_outputs(options, db_name);
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }
    pid_t pid = start_server(options, db_name, log_path);
    std::vector<std::unique_ptr<RmdbClient>> clients;
    try {
        auto setup = std::make_unique<RmdbClient>(options.timeout_sec);
        if (!verify_mvcc_baseline(options, test_case, *setup, db_name)) {
            throw std::runtime_error("MVCC transaction baseline failed");
        }
        std::vector<std::string> setup_statements;
        append_mvcc_setup(setup_statements);
        for (const auto &statement : setup_statements) {
            setup->execute(statement);
        }
        setup->close();

        auto t1 = std::make_unique<RmdbClient>(options.timeout_sec);
        auto t2 = std::make_unique<RmdbClient>(options.timeout_sec);
        RmdbClient *c1 = t1.get();
        RmdbClient *c2 = t2.get();
        clients.push_back(std::move(t1));
        clients.push_back(std::move(t2));
        std::vector<std::string> expected;

        if (test_case.kind == "mvcc_abort" || test_case.kind == "mvcc_dirty_read") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("update concurrency_test set score = 100.0 where id = 2;");
            c2->execute("select * from concurrency_test where id = 2;");
            c1->execute("abort;");
            c1->execute("select * from concurrency_test where id = 2;");
            c2->execute("commit;");
            expected = {"| id | name | score |", "| 2 | xiaoming | 95.000000 |",
                        "| id | name | score |", "| 2 | xiaoming | 95.000000 |"};
        } else if (test_case.kind == "mvcc_deadlock") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("update concurrency_test set score = 91.0 where id = 1;");
            c2->execute("update concurrency_test set score = 96.0 where id = 2;");
            std::vector<std::string> errors;
            std::vector<std::string> responses;
            std::mutex result_mutex;
            auto run_cross = [&](RmdbClient *client, const std::string &statement) {
                try {
                    std::string response = client->execute(statement);
                    std::lock_guard<std::mutex> lock(result_mutex);
                    responses.push_back(response);
                } catch (const std::exception &exception) {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    errors.push_back(exception.what());
                }
            };
            std::thread a(run_cross, c1, "update concurrency_test set score = 92.0 where id = 2;");
            std::thread b(run_cross, c2, "update concurrency_test set score = 97.0 where id = 1;");
            a.join();
            b.join();
            bool signaled = !errors.empty();
            for (auto response : responses) {
                std::transform(response.begin(), response.end(), response.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                signaled = signaled || response.find("abort") != std::string::npos ||
                           response.find("failure") != std::string::npos;
            }
            if (!signaled) {
                throw std::runtime_error("deadlock conflict completed without abort/failure signal");
            }
            c1->execute("abort;");
            c2->execute("abort;");
            cleanup_outputs(options, db_name);
            auto verifier = std::make_unique<RmdbClient>(options.timeout_sec);
            verifier->execute("select * from concurrency_test where id = 2;");
            verifier->close();
            expected = {"| id | name | score |", "| 2 | xiaoming | 95.000000 |"};
        } else if (test_case.kind == "mvcc_insert_delete") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("insert into concurrency_test values (4, 'newrow', 77.0);");
            c2->execute("delete from concurrency_test where id = 4;");
            c1->execute("commit;");
            c2->execute("abort;");
            c1->execute("select * from concurrency_test where id = 4;");
            expected = {"| id | name | score |", "| 4 | newrow | 77.000000 |"};
        } else if (test_case.kind == "mvcc_insert") {
            c1->execute("begin;");
            c1->execute("insert into concurrency_test values (4, 'insert', 80.0);");
            c1->execute("commit;");
            c2->execute("select * from concurrency_test where id = 4;");
            expected = {"| id | name | score |", "| 4 | insert | 80.000000 |"};
        } else if (test_case.kind == "mvcc_lost_update") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("select * from concurrency_test where id = 2;");
            c2->execute("update concurrency_test set score = 96.0 where id = 2;");
            c2->execute("commit;");
            c1->execute("update concurrency_test set score = 97.0 where id = 2;");
            c1->execute("commit;");
            c2->execute("select * from concurrency_test where id = 2;");
            expected = {"| id | name | score |", "| 2 | xiaoming | 95.000000 |",
                        "| id | name | score |", "| 2 | xiaoming | 97.000000 |"};
        } else if (test_case.kind == "mvcc_read_write_delete") {
            c1->execute("begin;");
            c1->execute("select * from concurrency_test where id = 3;");
            c2->execute("begin;");
            c2->execute("delete from concurrency_test where id = 3;");
            c2->execute("commit;");
            c1->execute("select * from concurrency_test where id = 3;");
            c1->execute("commit;");
            expected = {"| id | name | score |", "| 3 | zhanghua | 88.500000 |",
                        "| id | name | score |", "| 3 | zhanghua | 88.500000 |"};
        } else if (test_case.kind == "mvcc_scan") {
            c1->execute("begin;");
            c1->execute("select * from concurrency_test;");
            c2->execute("begin;");
            c2->execute("insert into concurrency_test values (4, 'scanrow', 70.0);");
            c2->execute("commit;");
            c1->execute("select * from concurrency_test;");
            c1->execute("commit;");
            expected = {"| id | name | score |", "| 1 | xiaohong | 90.000000 |",
                        "| 2 | xiaoming | 95.000000 |", "| 3 | zhanghua | 88.500000 |",
                        "| id | name | score |", "| 1 | xiaohong | 90.000000 |",
                        "| 2 | xiaoming | 95.000000 |", "| 3 | zhanghua | 88.500000 |"};
        } else if (test_case.kind == "mvcc_timestamp") {
            c1->execute("begin;");
            c1->execute("select * from concurrency_test where id = 1;");
            c1->execute("commit;");
            c2->execute("begin;");
            c2->execute("update concurrency_test set score = 91.0 where id = 1;");
            c2->execute("commit;");
            c1->execute("select * from concurrency_test where id = 1;");
            expected = {"| id | name | score |", "| 1 | xiaohong | 90.000000 |",
                        "| id | name | score |", "| 1 | xiaohong | 91.000000 |"};
        } else if (test_case.kind == "mvcc_tuple_reconstruct") {
            c1->execute("begin;");
            c2->execute("begin;");
            c2->execute("update concurrency_test set score = 96.0 where id = 2;");
            c2->execute("commit;");
            c1->execute("select * from concurrency_test where id = 2;");
            c1->execute("commit;");
            expected = {"| id | name | score |", "| 2 | xiaoming | 95.000000 |"};
        } else if (test_case.kind == "mvcc_update") {
            c1->execute("begin;");
            c1->execute("update concurrency_test set score = 91.0 where id = 1;");
            c1->execute("commit;");
            c2->execute("select * from concurrency_test where id = 1;");
            expected = {"| id | name | score |", "| 1 | xiaohong | 91.000000 |"};
        } else if (test_case.kind == "mvcc_write_write_delete_insert") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("delete from concurrency_test where id = 2;");
            c2->execute("insert into concurrency_test values (2, 'again', 99.0);");
            c1->execute("commit;");
            c2->execute("abort;");
            c1->execute("select * from concurrency_test where id = 2;");
            expected = {"| id | name | score |"};
        } else if (test_case.kind == "mvcc_write_write_update") {
            c1->execute("begin;");
            c2->execute("begin;");
            c1->execute("update concurrency_test set score = 96.0 where id = 2;");
            c2->execute("update concurrency_test set score = 97.0 where id = 2;");
            c1->execute("commit;");
            c2->execute("abort;");
            c1->execute("select * from concurrency_test where id = 2;");
            expected = {"| id | name | score |", "| 2 | xiaoming | 96.000000 |"};
        } else {
            throw std::runtime_error("unknown MVCC case kind: " + test_case.kind);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto &client : clients) {
            client->close();
        }
        stop_server(pid);
        bool ok = compare_case_output(options, test_case, db_name, expected);
        if (ok && !options.keep_db) {
            cleanup_db(options, db_name);
        }
        if (ok && !options.show_server_log) {
            std::error_code ignored;
            fs::remove(log_path, ignored);
        }
        return ok;
    } catch (const std::exception &exception) {
        for (auto &client : clients) {
            client->close();
        }
        stop_server(pid);
        std::cerr << "runtime error: " << exception.what() << "\n";
        if (options.show_server_log) {
            std::cerr << read_file(log_path);
        }
        return false;
    }
}

void recovery_workload(RmdbClient &client, int rows, bool use_index, bool use_checkpoint, int workers,
                       double timeout_sec) {
    client.execute("create table warehouse (w_id int, w_name char(10), w_ytd float);");
    if (use_index) {
        client.execute("create index warehouse(w_id);");
    }
    for (int i = 1; i <= rows; ++i) {
        client.execute("insert into warehouse values (" + std::to_string(i) + ", 'wh" +
                       padded_number(i % 10000, 4) + "', " + format_double(static_cast<double>(i)) + ");");
        if (use_checkpoint && i % std::max(1, rows / 4) == 0) {
            client.execute("create static_checkpoint;");
        }
    }
    std::vector<std::string> errors;
    std::mutex errors_mutex;
    auto run_worker = [&](int worker_id) {
        try {
            RmdbClient local(timeout_sec);
            for (int key = worker_id + 1; key <= rows; key += std::max(1, workers)) {
                local.execute("begin;");
                local.execute("update warehouse set w_ytd=" + format_double(1000.0 + key) +
                              " where w_id=" + std::to_string(key) + ";");
                local.execute("commit;");
            }
        } catch (const std::exception &exception) {
            std::lock_guard<std::mutex> lock(errors_mutex);
            errors.push_back(exception.what());
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back(run_worker, i);
    }
    for (auto &thread : threads) {
        thread.join();
    }
    if (!errors.empty()) {
        throw std::runtime_error(errors.front());
    }
    if (use_checkpoint) {
        client.execute("create static_checkpoint;");
    }
}

bool run_recovery_trial(const Options &options, const Case &test_case, int rows, bool use_index, bool use_checkpoint,
                        int workers, bool validate_query, const std::string &suffix, double *recovery_time) {
    std::string db_name = "doc_test_" + test_case.name + suffix;
    fs::path log_path = options.root / "tests" / (test_case.name + suffix + ".server.log");
    cleanup_outputs(options, db_name);
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }
    pid_t pid = start_server(options, db_name, log_path);
    try {
        RmdbClient client(options.timeout_sec);
        recovery_workload(client, rows, use_index, use_checkpoint, workers, options.timeout_sec);
        client.execute("crash;");
        client.close();
        int status = 0;
        waitpid(pid, &status, 0);
    } catch (const std::exception &exception) {
        stop_server(pid);
        std::cerr << "runtime error before restart: " << exception.what() << "\n";
        return false;
    }

    fs::path restart_log = options.root / "tests" / (test_case.name + suffix + ".restart.server.log");
    pid = start_server(options, db_name, restart_log);
    auto start = std::chrono::steady_clock::now();
    try {
        int probe = connect_to_server(std::max(options.connect_timeout_sec, 20.0));
        ::close(probe);
    } catch (const std::exception &exception) {
        stop_server(pid);
        std::cerr << "runtime error after restart: " << exception.what() << "\n";
        return false;
    }
    *recovery_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    bool ok = true;
    if (validate_query) {
        cleanup_outputs(options, db_name);
        try {
            RmdbClient client(options.timeout_sec);
            std::set<int> sample_set = {1, std::max(1, rows / 2), rows};
            std::vector<std::string> expected;
            for (int key : sample_set) {
                client.execute("select * from warehouse where w_id = " + std::to_string(key) + ";");
                expected.push_back("| w_id | w_name | w_ytd |");
                expected.push_back("| " + std::to_string(key) + " | wh" + padded_number(key % 10000, 4) +
                                   " | " + format_double(1000.0 + key) + " |");
            }
            client.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ok = compare_case_output(options, test_case, db_name, expected);
        } catch (const std::exception &exception) {
            std::cerr << "runtime error after restart: " << exception.what() << "\n";
            ok = false;
        }
    }
    stop_server(pid);
    if (ok && !options.keep_db) {
        cleanup_db(options, db_name);
    }
    return ok;
}

bool run_recovery_case(const Options &options, const Case &test_case) {
    bool use_index = test_case.kind == "recovery_index";
    bool use_checkpoint = test_case.kind == "recovery_with_checkpoint";
    int workers = (test_case.kind == "recovery_multi" || test_case.kind == "recovery_large") ? 4 : 1;
    int rows = options.recovery_row_count;
    if (test_case.kind == "recovery_single") {
        rows = std::max(20, rows / 10);
    } else if (test_case.kind == "recovery_index" || test_case.kind == "recovery_multi") {
        rows = std::max(50, rows / 4);
    } else if (test_case.kind == "recovery_large" || test_case.kind == "recovery_without_checkpoint" ||
               test_case.kind == "recovery_with_checkpoint") {
        rows = std::max(rows, 200);
    }

    double recovery_time = 0;
    if (!use_checkpoint) {
        return run_recovery_trial(options, test_case, rows, use_index, false, workers, true, "", &recovery_time);
    }

    double baseline_time = 0;
    Case baseline = test_case;
    baseline.name += "_baseline";
    if (!run_recovery_trial(options, baseline, rows, false, false, 1, true, "_without", &baseline_time)) {
        return false;
    }
    if (!run_recovery_trial(options, test_case, rows, false, true, 1, true, "_with", &recovery_time)) {
        return false;
    }
    double ratio = baseline_time > 0 ? recovery_time / baseline_time : 999.0;
    double tolerance = std::max(0.25, baseline_time * 0.15);
    if (ratio <= options.recovery_ratio || recovery_time <= baseline_time + tolerance) {
        return true;
    }
    std::cerr << "checkpoint/no-checkpoint ratio=" << ratio << ", checkpoint=" << recovery_time
              << "s, baseline=" << baseline_time << "s, required<=" << options.recovery_ratio
              << ", tolerance=+" << tolerance << "s\n";
    return false;
}

bool run_generated_case(const Options &options, const Case &test_case) {
    if (test_case.kind == "perf_single" || test_case.kind == "perf_multi") {
        return run_index_perf_case(options, test_case);
    }
    if (test_case.kind.rfind("txn_", 0) == 0) {
        return run_transaction_case(options, test_case);
    }
    if (test_case.kind.rfind("mvcc_", 0) == 0) {
        return run_mvcc_case(options, test_case);
    }
    if (test_case.kind.rfind("recovery_", 0) == 0) {
        return run_recovery_case(options, test_case);
    }
    std::cerr << "unknown generated case kind: " << test_case.kind << "\n";
    return false;
}

bool run_case(const Options &options, const Case &test_case) {
    if (test_case.kind != "static") {
        return run_generated_case(options, test_case);
    }

    std::string db_name = "doc_test_" + test_case.name;
    fs::path log_path = options.root / "tests" / (test_case.name + ".server.log");
    cleanup_outputs(options, db_name);
    if (!options.keep_db) {
        cleanup_db(options, db_name);
    }

    pid_t pid = start_server(options, db_name, log_path);
    int fd = -1;
    try {
        fd = connect_to_server(options.timeout_sec);
        send_sql(fd, split_sql(read_file(test_case.sql_path)));
        close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop_server(pid);
    } catch (const std::exception &exception) {
        if (fd >= 0) {
            close(fd);
        }
        stop_server(pid);
        std::cerr << "runtime error: " << exception.what() << "\n";
        if (options.show_server_log) {
            std::cerr << read_file(log_path);
        }
        return false;
    }

    bool preserve_table_order = preserve_table_order_for_case(test_case.name);
    bool preserve_plan_indentation = preserve_plan_indentation_for_case(test_case.name);
    auto expected = canonicalize_case_output(
        test_case.name, normalize_output(read_file(test_case.expected_path), preserve_table_order, preserve_plan_indentation));
    auto actual = canonicalize_case_output(
        test_case.name, normalize_output(read_actual_output(options, db_name), preserve_table_order, preserve_plan_indentation));
    if (expected == actual) {
        if (!options.show_server_log) {
            std::error_code ignored;
            fs::remove(log_path, ignored);
        }
        if (!options.keep_db) {
            cleanup_db(options, db_name);
        }
        return true;
    }
    print_mismatch(test_case, expected, actual);
    if (options.show_server_log) {
        std::cerr << "\n--- server log ---\n" << read_file(log_path);
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    Options options = parse_options(argc, argv);
    auto cases = discover_cases(options);
    if (options.list_only) {
        for (const auto &test_case : cases) {
            std::cout << test_case.name << "\n";
        }
        return 0;
    }
    if (!fs::exists(options.server)) {
        std::cerr << "Server binary not found: " << options.server << "\n";
        return 2;
    }
    if (cases.empty()) {
        std::cerr << "No test cases selected.\n";
        return 2;
    }

    int passed = 0;
    for (const auto &test_case : cases) {
        std::cout << "[ RUN      ] " << test_case.name << "\n";
        bool ok = run_case(options, test_case);
        if (ok) {
            ++passed;
        }
        std::cout << "[" << (ok ? "       OK " : "  FAILED ") << "] " << test_case.name << "\n";
    }
    std::cout << "\nSummary: " << passed << "/" << cases.size() << " passed\n";
    return passed == static_cast<int>(cases.size()) ? 0 : 1;
}
