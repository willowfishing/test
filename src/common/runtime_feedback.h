#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace rmdb {

constexpr uint32_t kInvalidRuntimeNodeId = UINT32_MAX;

enum class RuntimeNodeKind : uint8_t {
    kOther = 0,
    kSeqScan,
    kIndexScan,
    kJoin,
    kCountIndex,
    kMinMaxIndex,
    kAggregate,
    kSort,
    kLimit,
    kDml,
};

struct RuntimeScanFeedback {
    std::atomic<uint64_t> executions{0};
    std::atomic<uint64_t> rows_scanned{0};
    std::atomic<uint64_t> rows_visible{0};
    std::atomic<uint64_t> rows_output{0};
    std::atomic<uint64_t> index_entries{0};
    std::atomic<uint64_t> heap_fetches{0};
    std::atomic<uint64_t> elapsed_ns{0};
};

constexpr uint64_t kScanFeedbackMinExecutions = 8;
constexpr double kScanFeedbackHighTableRatio = 0.30;
constexpr uint64_t kScanFeedbackSmallTablePages = 8;
constexpr double kScanFeedbackGoodCoveringFallbackRatio = 0.05;

struct RuntimeScanFeedbackSnapshot {
    uint64_t executions = 0;
    uint64_t rows_scanned = 0;
    uint64_t rows_visible = 0;
    uint64_t rows_output = 0;
    uint64_t index_entries = 0;
    uint64_t heap_fetches = 0;
    uint64_t elapsed_ns = 0;
};

struct RuntimeJoinFeedback {
    std::atomic<uint64_t> executions{0};
    std::atomic<uint64_t> left_rows{0};
    std::atomic<uint64_t> right_rows{0};
    std::atomic<uint64_t> output_rows{0};
    std::atomic<uint64_t> probes{0};
    std::atomic<uint64_t> elapsed_ns{0};
    std::atomic<uint64_t> algo_nlj{0};
    std::atomic<uint64_t> algo_inlj{0};
    std::atomic<uint64_t> algo_hash{0};
    std::atomic<uint64_t> algo_merge{0};
};

struct RuntimeNodeFeedback {
    RuntimeNodeKind kind = RuntimeNodeKind::kOther;
    uint32_t node_id = kInvalidRuntimeNodeId;
    RuntimeScanFeedback scan;
    RuntimeJoinFeedback join;

    RuntimeNodeFeedback(RuntimeNodeKind kind_, uint32_t node_id_) : kind(kind_), node_id(node_id_) {}
};

struct RuntimeFeedbackStore {
    std::vector<std::shared_ptr<RuntimeNodeFeedback>> nodes;

    std::shared_ptr<RuntimeNodeFeedback> get(uint32_t node_id) const {
        if (node_id >= nodes.size()) {
            return nullptr;
        }
        return nodes[node_id];
    }

    std::shared_ptr<RuntimeNodeFeedback> ensure(uint32_t node_id, RuntimeNodeKind kind) {
        if (nodes.size() <= node_id) {
            nodes.resize(static_cast<size_t>(node_id) + 1);
        }
        if (nodes[node_id] == nullptr) {
            nodes[node_id] = std::make_shared<RuntimeNodeFeedback>(kind, node_id);
        }
        return nodes[node_id];
    }
};

inline void record_scan_feedback(const std::shared_ptr<RuntimeNodeFeedback> &feedback,
                                 uint64_t rows_scanned,
                                 uint64_t rows_visible,
                                 uint64_t rows_output,
                                 uint64_t index_entries,
                                 uint64_t heap_fetches,
                                 uint64_t elapsed_ns) {
    if (feedback == nullptr) {
        return;
    }
    feedback->scan.executions.fetch_add(1, std::memory_order_relaxed);
    feedback->scan.rows_scanned.fetch_add(rows_scanned, std::memory_order_relaxed);
    feedback->scan.rows_visible.fetch_add(rows_visible, std::memory_order_relaxed);
    feedback->scan.rows_output.fetch_add(rows_output, std::memory_order_relaxed);
    feedback->scan.index_entries.fetch_add(index_entries, std::memory_order_relaxed);
    feedback->scan.heap_fetches.fetch_add(heap_fetches, std::memory_order_relaxed);
    feedback->scan.elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

inline RuntimeScanFeedbackSnapshot load_scan_feedback_snapshot(
    const std::shared_ptr<RuntimeNodeFeedback> &feedback) {
    RuntimeScanFeedbackSnapshot snapshot;
    if (feedback == nullptr) {
        return snapshot;
    }
    snapshot.executions = feedback->scan.executions.load(std::memory_order_relaxed);
    snapshot.rows_scanned = feedback->scan.rows_scanned.load(std::memory_order_relaxed);
    snapshot.rows_visible = feedback->scan.rows_visible.load(std::memory_order_relaxed);
    snapshot.rows_output = feedback->scan.rows_output.load(std::memory_order_relaxed);
    snapshot.index_entries = feedback->scan.index_entries.load(std::memory_order_relaxed);
    snapshot.heap_fetches = feedback->scan.heap_fetches.load(std::memory_order_relaxed);
    snapshot.elapsed_ns = feedback->scan.elapsed_ns.load(std::memory_order_relaxed);
    return snapshot;
}

inline double runtime_feedback_ratio(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace rmdb
