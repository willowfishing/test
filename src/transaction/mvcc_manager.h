#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common.h"
#include "record/rm_defs.h"
#include "system/sm.h"
#include "transaction/transaction.h"

struct MvccRecord {
    Rid rid;
    std::unique_ptr<RmRecord> record;
};

class MvccManager {
   public:
    static void Begin(Transaction *txn);
    static void OnCommit(Transaction *txn, timestamp_t commit_ts);
    static void OnAbort(Transaction *txn);

    static void AppendVersion(const std::string &tab_name, const Rid &rid, Transaction *txn, bool deleted,
                              const RmRecord &record);
    static void EnsureBaseVersion(const std::string &tab_name, const Rid &rid, const RmRecord &record);

    static std::vector<MvccRecord> CollectVisibleRecords(SmManager *sm_manager, const std::string &tab_name,
                                                         Transaction *txn);

    static void RegisterPredicateRead(Transaction *txn, const std::string &tab_name,
                                      const std::vector<ColMeta> &cols, const std::vector<Condition> &conds);
    static void RegisterRecordRead(Transaction *txn, const std::string &tab_name, const Rid &rid);

    static void CheckWrite(Transaction *txn, const std::string &tab_name, const Rid &rid,
                           const std::vector<ColMeta> &cols, const std::vector<const RmRecord *> &records);
};
