#include "transaction/mvcc_manager.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "errors.h"
#include "record/rm_scan.h"

namespace {

struct Version {
    Rid rid;
    txn_id_t txn_id;
    bool committed{false};
    bool aborted{false};
    bool deleted{false};
    timestamp_t commit_ts{INVALID_TS};
    std::shared_ptr<RmRecord> record;
};

struct PredicateRead {
    std::string tab_name;
    std::vector<ColMeta> cols;
    std::vector<Condition> conds;
};

struct WriteEvent {
    std::string tab_name;
    std::vector<std::shared_ptr<RmRecord>> records;
};

struct TxInfo {
    IsolationLevel isolation{IsolationLevel::SERIALIZABLE};
    timestamp_t start_ts{0};
    bool aborted{false};
    bool committed{false};
    timestamp_t commit_ts{INVALID_TS};
    std::set<std::string> read_records;
    std::vector<PredicateRead> predicates;
    std::vector<WriteEvent> writes;
};

std::mutex mvcc_latch;
std::unordered_map<std::string, std::map<std::pair<int, int>, std::vector<Version>>> versions;
std::unordered_map<txn_id_t, TxInfo> tx_infos;
std::set<std::pair<txn_id_t, txn_id_t>> rw_edges;

std::pair<int, int> rid_key(const Rid &rid) { return {rid.page_no, rid.slot_no}; }

std::string record_key(const std::string &tab_name, const Rid &rid) {
    return tab_name + "#" + std::to_string(rid.page_no) + "#" + std::to_string(rid.slot_no);
}

bool is_active_state(TransactionState state) {
    return state != TransactionState::COMMITTED && state != TransactionState::ABORTED;
}

TxInfo &ensure_tx_info(Transaction *txn) {
    auto &info = tx_infos[txn->get_transaction_id()];
    info.isolation = txn->get_isolation_level();
    info.start_ts = txn->get_start_ts();
    return info;
}

bool visible_to(const Version &version, Transaction *txn) {
    if (txn == nullptr) {
        return version.committed && !version.aborted;
    }
    if (version.aborted) {
        return false;
    }
    if (version.txn_id == txn->get_transaction_id()) {
        return true;
    }
    return version.committed && version.commit_ts <= txn->get_start_ts();
}

std::unique_ptr<RmRecord> visible_record_locked(const std::string &tab_name, const Rid &rid, Transaction *txn,
                                                const RmRecord *fallback) {
    auto tab_it = versions.find(tab_name);
    if (tab_it != versions.end()) {
        auto ver_it = tab_it->second.find(rid_key(rid));
        if (ver_it != tab_it->second.end()) {
            for (auto it = ver_it->second.rbegin(); it != ver_it->second.rend(); ++it) {
                if (visible_to(*it, txn)) {
                    if (it->deleted) {
                        return nullptr;
                    }
                    return std::make_unique<RmRecord>(*it->record);
                }
            }
            return nullptr;
        }
    }
    if (fallback != nullptr) {
        return std::make_unique<RmRecord>(*fallback);
    }
    return nullptr;
}

int compare_value(ColType type, const char *lhs, const char *rhs, int len) {
    if (type == TYPE_INT) {
        int a;
        int b;
        memcpy(&a, lhs, sizeof(int));
        memcpy(&b, rhs, sizeof(int));
        return (a > b) - (a < b);
    }
    if (type == TYPE_FLOAT) {
        float a;
        float b;
        memcpy(&a, lhs, sizeof(float));
        memcpy(&b, rhs, sizeof(float));
        return (a > b) - (a < b);
    }
    return memcmp(lhs, rhs, len);
}

bool eval_cond(const std::vector<ColMeta> &cols, const RmRecord &rec, const Condition &cond) {
    auto lhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
    });
    if (lhs == cols.end() && !cond.lhs_col.tab_name.empty()) {
        lhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) { return col.name == cond.lhs_col.col_name; });
    }
    if (lhs == cols.end()) {
        return false;
    }

    const char *rhs_data = nullptr;
    ColType rhs_type = lhs->type;
    int rhs_len = lhs->len;
    if (cond.is_rhs_val) {
        rhs_data = cond.rhs_val.raw->data;
    } else {
        auto rhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
        });
        if (rhs == cols.end() && !cond.rhs_col.tab_name.empty()) {
            rhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) { return col.name == cond.rhs_col.col_name; });
        }
        if (rhs == cols.end()) {
            return false;
        }
        rhs_data = rec.data + rhs->offset;
        rhs_type = rhs->type;
        rhs_len = rhs->len;
    }
    if (lhs->type != rhs_type) {
        return false;
    }

    int cmp = compare_value(lhs->type, rec.data + lhs->offset, rhs_data, rhs_len);
    switch (cond.op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
    }
    return false;
}

bool eval_conds(const std::vector<ColMeta> &cols, const RmRecord &rec, const std::vector<Condition> &conds) {
    return std::all_of(conds.begin(), conds.end(), [&](const Condition &cond) { return eval_cond(cols, rec, cond); });
}

bool has_path_locked(txn_id_t from, txn_id_t to, std::set<txn_id_t> &seen) {
    if (from == to) {
        return true;
    }
    if (!seen.insert(from).second) {
        return false;
    }
    for (auto &edge : rw_edges) {
        if (edge.first == from && has_path_locked(edge.second, to, seen)) {
            return true;
        }
    }
    return false;
}

bool committed_before_locked(txn_id_t lhs, txn_id_t rhs) {
    auto lhs_it = tx_infos.find(lhs);
    if (lhs_it == tx_infos.end() || !lhs_it->second.committed) {
        return false;
    }
    auto rhs_it = tx_infos.find(rhs);
    if (rhs_it == tx_infos.end() || !rhs_it->second.committed) {
        return true;
    }
    return lhs_it->second.commit_ts < rhs_it->second.commit_ts;
}

bool dangerous_structure_locked(txn_id_t from, txn_id_t to) {
    for (auto &edge : rw_edges) {
        if (edge.second == from) {
            txn_id_t tin = edge.first;
            txn_id_t tout = to;
            if (tin == tout || committed_before_locked(tout, tin)) {
                return true;
            }
        }
        if (edge.first == to) {
            txn_id_t tin = from;
            txn_id_t tout = edge.second;
            if (tin == tout || committed_before_locked(tout, tin)) {
                return true;
            }
        }
    }
    return false;
}

void add_rw_edge_or_abort_locked(Transaction *current, txn_id_t from, txn_id_t to) {
    if (from == to) {
        return;
    }
    auto [_, inserted] = rw_edges.insert({from, to});
    if (!inserted) {
        return;
    }
    if (current != nullptr && dangerous_structure_locked(from, to)) {
        throw TransactionAbortException(current->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
    std::set<txn_id_t> seen;
    if (current != nullptr && has_path_locked(to, from, seen)) {
        throw TransactionAbortException(current->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }
}

bool version_invisible_to(const Version &version, Transaction *txn) {
    if (txn == nullptr || version.aborted || version.txn_id == txn->get_transaction_id()) {
        return false;
    }
    if (!version.committed) {
        return true;
    }
    return version.commit_ts > txn->get_start_ts();
}

}  // namespace

void MvccManager::Begin(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    ensure_tx_info(txn);
}

void MvccManager::OnCommit(Transaction *txn, timestamp_t commit_ts) {
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    auto &info = ensure_tx_info(txn);
    info.committed = true;
    info.commit_ts = commit_ts;
    for (auto &tab_entry : versions) {
        for (auto &rid_entry : tab_entry.second) {
            for (auto &version : rid_entry.second) {
                if (version.txn_id == txn->get_transaction_id() && !version.aborted) {
                    version.committed = true;
                    version.commit_ts = commit_ts;
                }
            }
        }
    }
}

void MvccManager::OnAbort(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    txn_id_t txn_id = txn->get_transaction_id();
    for (auto &tab_entry : versions) {
        for (auto &rid_entry : tab_entry.second) {
            auto &vec = rid_entry.second;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const Version &version) { return version.txn_id == txn_id; }),
                      vec.end());
        }
    }
    auto info_it = tx_infos.find(txn_id);
    if (info_it != tx_infos.end()) {
        info_it->second.aborted = true;
        info_it->second.read_records.clear();
        info_it->second.predicates.clear();
        info_it->second.writes.clear();
    }
    for (auto it = rw_edges.begin(); it != rw_edges.end();) {
        if (it->first == txn_id || it->second == txn_id) {
            it = rw_edges.erase(it);
        } else {
            ++it;
        }
    }
}

void MvccManager::AppendVersion(const std::string &tab_name, const Rid &rid, Transaction *txn, bool deleted,
                                const RmRecord &record) {
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    ensure_tx_info(txn);
    versions[tab_name][rid_key(rid)].push_back(
        Version{rid, txn->get_transaction_id(), false, false, deleted, INVALID_TS, std::make_shared<RmRecord>(record)});
}

void MvccManager::EnsureBaseVersion(const std::string &tab_name, const Rid &rid, const RmRecord &record) {
    std::lock_guard<std::mutex> guard(mvcc_latch);
    auto &vec = versions[tab_name][rid_key(rid)];
    if (vec.empty()) {
        vec.push_back(Version{rid, INVALID_TXN_ID, true, false, false, 0, std::make_shared<RmRecord>(record)});
    }
}

std::vector<MvccRecord> MvccManager::CollectVisibleRecords(SmManager *sm_manager, const std::string &tab_name,
                                                           Transaction *txn) {
    std::map<std::pair<int, int>, std::unique_ptr<RmRecord>> physical;
    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        try {
            physical[rid_key(scan.rid())] = fh->get_record(scan.rid(), nullptr);
        } catch (RecordNotFoundError &) {
        }
    }

    std::lock_guard<std::mutex> guard(mvcc_latch);
    std::set<std::pair<int, int>> keys;
    for (auto &entry : physical) {
        keys.insert(entry.first);
    }
    auto tab_it = versions.find(tab_name);
    if (tab_it != versions.end()) {
        for (auto &entry : tab_it->second) {
            keys.insert(entry.first);
        }
    }

    std::vector<MvccRecord> result;
    for (auto &key : keys) {
        Rid rid{key.first, key.second};
        const RmRecord *fallback = nullptr;
        auto physical_it = physical.find(key);
        if (physical_it != physical.end()) {
            fallback = physical_it->second.get();
        }
        auto rec = visible_record_locked(tab_name, rid, txn, fallback);
        if (rec != nullptr) {
            result.push_back(MvccRecord{rid, std::move(rec)});
        }
    }
    return result;
}

void MvccManager::RegisterPredicateRead(Transaction *txn, const std::string &tab_name, const std::vector<ColMeta> &cols,
                                        const std::vector<Condition> &conds) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE ||
        !is_active_state(txn->get_state())) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    auto &info = ensure_tx_info(txn);
    info.predicates.push_back(PredicateRead{tab_name, cols, conds});

    // A later SELECT must also notice an UPDATE that moved a tuple out of
    // the predicate.  Looking only at the writer's newest MVCC version is
    // insufficient in that case, because only the old image matches.  Keep
    // the old and new images recorded by CheckWrite and test both here.
    for (auto &entry : tx_infos) {
        txn_id_t writer_id = entry.first;
        TxInfo &writer = entry.second;
        if (writer_id == txn->get_transaction_id() || writer.aborted ||
            writer.isolation != IsolationLevel::SERIALIZABLE) {
            continue;
        }
        if (writer.committed && writer.commit_ts <= txn->get_start_ts()) {
            continue;
        }
        bool matches = false;
        for (auto &write : writer.writes) {
            if (write.tab_name != tab_name) {
                continue;
            }
            for (auto &record : write.records) {
                if (record != nullptr && eval_conds(cols, *record, conds)) {
                    matches = true;
                    break;
                }
            }
            if (matches) {
                break;
            }
        }
        if (matches) {
            add_rw_edge_or_abort_locked(txn, txn->get_transaction_id(), writer_id);
        }
    }

    auto tab_it = versions.find(tab_name);
    if (tab_it == versions.end()) {
        return;
    }
    for (auto &rid_entry : tab_it->second) {
        for (auto &version : rid_entry.second) {
            if (version_invisible_to(version, txn) && eval_conds(cols, *version.record, conds)) {
                add_rw_edge_or_abort_locked(txn, txn->get_transaction_id(), version.txn_id);
            }
        }
    }
}

void MvccManager::RegisterRecordRead(Transaction *txn, const std::string &tab_name, const Rid &rid) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE ||
        !is_active_state(txn->get_state())) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    auto &info = ensure_tx_info(txn);
    info.read_records.insert(record_key(tab_name, rid));
}

void MvccManager::CheckWrite(Transaction *txn, const std::string &tab_name, const Rid &rid,
                             const std::vector<ColMeta> &cols, const std::vector<const RmRecord *> &records) {
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(mvcc_latch);
    ensure_tx_info(txn);
    auto tab_it = versions.find(tab_name);
    if (tab_it != versions.end()) {
        auto ver_it = tab_it->second.find(rid_key(rid));
        if (ver_it != tab_it->second.end() && !ver_it->second.empty()) {
            const Version &latest = ver_it->second.back();
            if (!latest.aborted && latest.txn_id != txn->get_transaction_id()) {
                if (!latest.committed || latest.commit_ts > txn->get_start_ts()) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }
    }

    if (txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    if (!records.empty()) {
        WriteEvent write{tab_name, {}};
        write.records.reserve(records.size());
        for (auto *record : records) {
            if (record != nullptr) {
                write.records.push_back(std::make_shared<RmRecord>(*record));
            }
        }
        if (!write.records.empty()) {
            tx_infos[txn->get_transaction_id()].writes.push_back(std::move(write));
        }
    }
    const std::string key = record_key(tab_name, rid);
    for (auto &entry : tx_infos) {
        txn_id_t reader_id = entry.first;
        TxInfo &reader = entry.second;
        if (reader_id == txn->get_transaction_id() || reader.aborted || reader.isolation != IsolationLevel::SERIALIZABLE) {
            continue;
        }
        if (reader.committed && reader.commit_ts <= txn->get_start_ts()) {
            continue;
        }
        bool matches = reader.read_records.count(key) > 0;
        if (!matches) {
            for (auto &predicate : reader.predicates) {
                if (predicate.tab_name != tab_name) {
                    continue;
                }
                for (auto *rec : records) {
                    if (rec != nullptr && eval_conds(predicate.cols, *rec, predicate.conds)) {
                        matches = true;
                        break;
                    }
                }
                if (matches) {
                    break;
                }
            }
        }
        if (matches) {
            add_rw_edge_or_abort_locked(txn, reader_id, txn->get_transaction_id());
        }
    }
}
