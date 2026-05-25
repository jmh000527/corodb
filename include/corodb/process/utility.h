// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file utility.h
 *  @brief Utility 处理器：DDL（CREATE/DROP TABLE/INDEX）。
 *
 *  对应 PostgreSQL 的 ProcessUtility（src/backend/tcop/utility.c）。
 *  Utility 命令不进 Executor，由这里直接对 Catalog/Storage/Table 执行。
 */

#pragma once

#include "corodb/plan/physical_plan.h"
#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief Utility（DDL）命令处理器。
     */
    class UtilityProcessor {
    public:
        UtilityProcessor(Catalog& catalog, StorageEngine& storage) noexcept : catalog_(catalog), storage_(storage) {
        }

        /** @brief 判断该物理计划是否为 utility（DDL） 计划。 */
        [[nodiscard]] static bool is_utility_plan(const PlanNode* plan) noexcept;

        /** @brief 执行 utility 计划（CREATE/DROP TABLE/INDEX）。 */
        void process(const PlanNode* plan);

    private:
        void create_table(const CreateTablePlan& plan);
        void create_index(const CreateIndexPlan& plan);
        void drop_table(const DropTablePlan& plan);
        void drop_index(const DropIndexPlan& plan);

        Catalog& catalog_;
        StorageEngine& storage_;
    };

} // namespace corodb
