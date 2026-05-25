// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file executor.h
 *  @brief 物理计划执行器（对应 PG 的 execMain）。
 *
 *  Executor 仅执行优化器返回的物理计划中的 DML/SELECT 算子；
 *  DDL（CREATE/DROP TABLE/INDEX）由 commands/UtilityProcessor 处理；
 *  EXPLAIN 由 commands/ExplainPrinter 处理；
 *  事务控制（BEGIN/COMMIT/ROLLBACK）由 tcop/TransactionController 处理。
 */

#pragma once

#include <generator>
#include <stdexcept>

#include "corodb/executor/execution_context.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief 写写冲突异常。
     *
     * 由事务并发控制（first-committer-wins）抛出，属于预期的业务异常，
     * 服务器不将其打印到 stderr，直接返回 ERROR 响应给客户端。
     */
    class WriteConflictError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief 物理算子执行器。
     *
     * 对外提供一个 run(plan) 接口；按 PlanNode 的具体子类型分派到内部
     * 算子实现。算子实现以协程形式 co_yield 出 Record，提供惰性流水线。
     *
     * 所有算子共享 ctx_：捕获 Session、RowLockManager、Catalog、Storage、
     * TransactionManager 引用。同一个 Executor 实例在生成器消费完毕前
     * 必须保持存活，以便维护 ctx_ 的有效性。
     */
    class Executor {
    public:
        /** @brief 构造时绑定执行上下文。 */
        explicit Executor(ExecutionContext ctx) noexcept : ctx_(ctx) {
        }

        /**
         * @brief 执行物理计划，返回行流。
         * @throws std::runtime_error 当遇到非 DML/SELECT 计划节点（如 DDL）时抛出。
         */
        [[nodiscard]] std::generator<Record> run(const PlanNode* plan);

        /** @brief Execute with profiling; populates stats for EXPLAIN ANALYZE. */
        [[nodiscard]] std::generator<Record> run_profiled(const PlanNode* plan, QueryStats& stats);

        [[nodiscard]] const ExecutionContext& context() const noexcept {
            return ctx_;
        }

    private:
        ExecutionContext ctx_;
    };

} // namespace corodb
