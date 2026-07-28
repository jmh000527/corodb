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
    struct SelectStmt;
    struct Record;

    /**
     * @brief 相关子查询求值器（由 QueryProcessor 注入）。
     *
     * 执行期逐外层行求值：先把子查询中引用外层表的列按 outer 行代换为字面量，
     * 再递归规划/执行（nested apply）。exists_only 时取到首行即短路。
     */
    class SubqueryRunner {
    public:
        virtual ~SubqueryRunner() = default;
        /** @return 单列结果值列表；exists_only 时非空即表示存在（至多含 1 个占位值）。 */
        virtual std::vector<Value> run_subquery(const SelectStmt& sub, const Record& outer, bool exists_only) = 0;
    };

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
        /// 相关子查询求值器（可为空；为空时遇到未解析子查询报错）。
        SubqueryRunner* subquery_runner{ nullptr };
    };

} // namespace corodb
