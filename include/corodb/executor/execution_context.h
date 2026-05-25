// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file execution_context.h
 *  @brief 显式传递的执行上下文，替代旧的线程局部 Session 绑定。
 *
 *  对应 PostgreSQL 的 EState：每条语句执行时携带的可变运行时上下文。
 */

#pragma once

#include <chrono>
#include <cstdint>

#include <memory>

#include "corodb/db/session.h"
#include "corodb/storage/table.h"

namespace corodb {

    class RowLockManager;
    class TransactionManager;
    class StorageEngine;

    /**
     * @brief 算子执行所需的全部上下文（显式参数，禁用 TLS）。
     *
     * - session：调用方会话；事务/隔离级别/写缓冲均通过它访问。
     * - row_locks：事务行锁管理器。
     * - catalog：表目录。
     * - storage：存储引擎。
     * - txn_manager：事务管理器（分配 commit_ts、读 ts 等）。
     */
    struct ExecutionContext {
        std::shared_ptr<Session> session;
        RowLockManager* row_locks{ nullptr };
        Catalog* catalog{ nullptr };
        StorageEngine* storage{ nullptr };
        TransactionManager* txn_manager{ nullptr };
        /// 查询截止时间（steady_clock）。默认 = epoch = 无超时。
        std::chrono::steady_clock::time_point deadline{};
    };

} // namespace corodb
