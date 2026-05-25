// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file transaction_controller.h
 *  @brief 事务控制语句处理器（BEGIN/COMMIT/ROLLBACK/SET TRANSACTION）。
 *
 *  对应 PostgreSQL 的 xact.c 高层入口（BeginTransactionBlock 等）。
 */

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "corodb/db/session.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/table.h"
#include "corodb/txn/row_lock_manager.h"
#include "corodb/txn/transaction_manager.h"

namespace corodb {

    /**
     * @brief 事务控制结果。
     */
    struct TxnControlResult {
        std::string message;
    };

    /**
     * @brief 处理 BEGIN / COMMIT / ROLLBACK / SET TRANSACTION 语句。
     */
    class TransactionController {
    public:
        TransactionController(TransactionManager& txn_manager, Catalog& catalog, StorageEngine& storage,
                              RowLockManager& row_locks, std::mutex& commit_apply_mutex) noexcept
            : txn_manager_(txn_manager), catalog_(catalog), storage_(storage), row_locks_(row_locks),
              commit_apply_mutex_(commit_apply_mutex) {
        }

        /**
         * @brief 若 stmt 是事务控制语句则处理并返回结果，否则返回 nullopt。
         */
        std::optional<TxnControlResult> handle(const Statement& stmt, Session& session);

        /**
         * @brief 为即将执行的语句（非事务控制语句）准备 snapshot_ts / auto_commit_ts。
         */
        void prepare_for_statement(const Statement& stmt, Session& session);

    private:
        TxnControlResult begin(Session& session);
        TxnControlResult commit(Session& session);
        TxnControlResult rollback(Session& session);
        TxnControlResult set_transaction(const SetTransactionStmt& stmt, Session& session);

        TransactionManager& txn_manager_;
        Catalog& catalog_;
        StorageEngine& storage_;
        RowLockManager& row_locks_;
        std::mutex& commit_apply_mutex_;
    };

} // namespace corodb
