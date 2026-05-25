// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file physical_planner.h @brief 物理计划生成器定义。 */

#pragma once

#include <memory>

#include "corodb/plan/logical_plan.h"
#include "corodb/plan/physical_plan.h"

namespace corodb {
    class Catalog;
    class StorageEngine;
} // namespace corodb

namespace corodb::opt {

    /** @brief 将逻辑计划翻译为可执行的物理计划。 */
    class PhysicalPlanner {
    public:
        /** @brief 构造仅用于纯计划测试的规划器。 */
        PhysicalPlanner();

        /** @brief 构造可处理真实 DDL 的规划器。 */
        PhysicalPlanner(Catalog& catalog, StorageEngine* storage);

        ~PhysicalPlanner();

        /** @brief 转换入口。 */
        [[nodiscard]] std::unique_ptr<PlanNode> plan(const LogicalPlan& lp);

    private:
        std::unique_ptr<PlanNode> visit(const LogicalPlan& lp);
        std::unique_ptr<PlanNode> build_scan(const LogicalScan& s);
        std::unique_ptr<PlanNode> build_filter(const LogicalFilter& f);
        std::unique_ptr<PlanNode> build_project(const LogicalProject& p);
        std::unique_ptr<PlanNode> build_join(const LogicalJoin& j);
        std::unique_ptr<PlanNode> build_aggregate(const LogicalAggregate& a);
        std::unique_ptr<PlanNode> build_sort(const LogicalSort& s);
        std::unique_ptr<PlanNode> build_limit(const LogicalLimit& l);
        std::unique_ptr<PlanNode> build_dml(const LogicalDML& d);
        std::unique_ptr<PlanNode> build_ddl(const LogicalDDL& d);

        Catalog* catalog_{ nullptr };
        StorageEngine* storage_{ nullptr };
    };

} // namespace corodb::opt
