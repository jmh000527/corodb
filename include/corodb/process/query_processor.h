// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file query_processor.h
 *  @brief 顶层 SQL 流水线（对应 PostgreSQL 的 postgres.c::exec_simple_query）。
 *
 *  parser → logical_planner → rules → physical_planner → 分派
 *      ├─ TransactionController（BEGIN/COMMIT/ROLLBACK/SET TX）
 *      ├─ ExplainPrinter（EXPLAIN）
 *      ├─ UtilityProcessor（CREATE/DROP TABLE/INDEX）
 *      └─ Executor（DML/SELECT 物理算子执行）
 */

#pragma once

#include <generator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "corodb/db/session.h"
#include "corodb/optimizer/logical/logical_planner.h"
#include "corodb/optimizer/logical/rule.h"
#include "corodb/optimizer/physical/physical_planner.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/process/transaction_controller.h"
#include "corodb/process/utility.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/table.h"
#include "corodb/txn/lock_manager.h"
#include "corodb/txn/row_lock_manager.h"
#include "corodb/txn/transaction_manager.h"

namespace corodb {

    class UserManager;

    /**
     * @brief 单条 SQL 的处理结果。
     */
    struct ProcessedQuery {
        std::optional<std::string> message;
        std::optional<std::generator<Record>> rows;
        std::shared_ptr<void> plan; // 物理计划（在 generator 消费完毕前需要保持存活）
        bool is_select{ false };
    };

    /**
     * @brief 物理计划缓存（LRU eviction，DDL 时全量失效）。
     */
    class PlanCache {
    public:
        /** @brief 根据标准化 SQL 查找缓存的计划，未命中返回 nullptr。 */
        std::shared_ptr<PlanNode> lookup(const std::string& normalized_sql);

        /** @brief 将标准化 SQL 及其计划插入缓存。 */
        void insert(const std::string& normalized_sql, std::shared_ptr<PlanNode> plan);

        /** @brief DDL 后全量清空（表结构变更使所有计划可能失效）。 */
        void invalidate_all();

        /** @brief 设置缓存条目上限（从 Config 读取）。 */
        void set_max_entries(std::size_t n) noexcept { max_entries_ = n; }

        /** @brief 返回缓存条目数。 */
        [[nodiscard]] std::size_t size() const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<PlanNode>> cache_;
        std::list<std::string> lru_;
        std::size_t max_entries_{ 128 };
    };

    /**
     * @brief 顶层 SQL 处理流水线。
     */
    class QueryProcessor {
    public:
        QueryProcessor(Catalog& catalog, StorageEngine& storage, TransactionManager& txn_manager,
                       LockManager& lock_manager, RowLockManager& row_locks, std::mutex& commit_apply_mutex,
                       UserManager& user_manager);

        /** @brief 完整流水线处理一条 SQL。 */
        ProcessedQuery run(const std::string& sql, std::shared_ptr<Session> session);

    private:
        std::unique_ptr<PlanNode> build_physical_plan(const Statement& stmt);

        /** @brief 语句 WHERE 中是否含未解析的 IN (SELECT ...) 子查询。 */
        [[nodiscard]] static bool stmt_has_subquery(const Statement& stmt);
        [[nodiscard]] static bool bool_has_subquery(const BoolExpr& e);

        /** @brief 递归执行并代换 WHERE 中的非相关 IN (SELECT ...) 子查询为字面量 IN 列表。 */
        void resolve_subqueries(Statement& stmt, const std::shared_ptr<Session>& session, int depth);
        void resolve_subqueries_in_bool(BoolExpr& e, const std::shared_ptr<Session>& session, int depth);

        /** @brief 构建 SHOW STATUS 的状态指标行生成器。 */
        [[nodiscard]] std::generator<Record> build_status_rows();

        Catalog& catalog_;
        StorageEngine& storage_;
        TransactionManager& txn_manager_;
        LockManager& lock_manager_;
        RowLockManager& row_locks_;
        UserManager& user_manager_;

        TransactionController txn_ctrl_;
        UtilityProcessor utility_;
        PlanCache plan_cache_;

        /** @brief 轻量 SQL 标准化：折叠空白，统一大小写。 */
        static std::string normalize_sql(const std::string& sql);
    };

} // namespace corodb
