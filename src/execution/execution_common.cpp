/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <vector>
#include <optional>
#include <cstring>

#include "transaction/transaction.h"
#include "system/sm_meta.h"
#include "common/common.h"
#include "common/config.h"

/**
 * @description: 检查写写冲突
 * 对于 SI/SER 隔离级别，检查给定记录是否与当前事务冲突。
 * @param {timestamp_t} tuple_ts 数据元组的时间戳（commit_ts或INVALID_TS表示未提交）
 * @param {Transaction*} txn 当前事务
 * @return {bool} 如果有冲突返回 true
 */
auto IsWriteWriteConflict(timestamp_t tuple_ts, Transaction *txn) -> bool {
    if (txn == nullptr) return false;
    if (tuple_ts == INVALID_TS) {
        // No timestamp means the record hasn't been modified by a committed txn
        return false;
    }
    return tuple_ts > txn->get_start_ts();
}

/**
 * @description: 通过撤销日志重建元组的旧版本
 * @param {const TabMeta*} schema 表的模式
 * @param {const RmRecord&} base_tuple 当前数据
 * @param {const TupleMeta&} base_meta 元组元数据（ts_, is_deleted_）
 * @param {const std::vector<UndoLog>&} undo_logs UndoLog 列表
 * @return {std::optional<RmRecord>} 重建的元组，如果被删除返回 nullopt
 */
auto ReconstructTuple(const TabMeta *schema, const RmRecord &base_tuple, const TupleMeta &base_meta,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<RmRecord> {
    if (schema == nullptr) {
        return base_tuple;
    }

    // Start with a copy of the base tuple
    RmRecord result(base_tuple.size);
    memcpy(result.data, base_tuple.data, base_tuple.size);

    // Apply undo logs in forward order to reconstruct older version
    for (const auto &undo_log : undo_logs) {
        if (undo_log.is_deleted_) {
            // The undo log marks this as deleted — the record didn't exist before
            return std::nullopt;
        }

        // Apply modified field reversions from undo log
        if (undo_log.tuple_test_ != nullptr) {
            // The old record is stored in tuple_test_ — restore field values
            memcpy(result.data, undo_log.tuple_test_->data, result.size);
        }

        // Apply field-level undo if modified_fields_ is populated
        if (!undo_log.modified_fields_.empty()) {
            for (size_t i = 0; i < undo_log.modified_fields_.size() && i < schema->cols.size(); i++) {
                if (undo_log.modified_fields_[i]) {
                    // This field was modified — restore old value
                    // In a full implementation, old values would be stored in undo_log.tuple_
                    // For now, tuple_test_ contains the full old record
                }
            }
        }
    }

    return result;
}
