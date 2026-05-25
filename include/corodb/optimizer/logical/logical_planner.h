// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file logical_planner.h @brief AST 到 LogicalPlan 的转换入口。 */

#pragma once

#include "corodb/ast/ast.h"
#include "corodb/plan/logical_plan.h"
#include "corodb/storage/table.h"

namespace corodb::opt {

    /** @brief 根据 AST 构建逻辑计划树。 */
    class LogicalPlanner {
    public:
        explicit LogicalPlanner(Catalog& catalog) : catalog_(catalog) {
        }

        /** @brief 将 AST 语句转换为逻辑计划。 */
        [[nodiscard]] LogicalPlanPtr plan(const Statement& stmt);

    private:
        [[nodiscard]] LogicalPlanPtr plan_select(const SelectStmt& select);
        [[nodiscard]] LogicalPlanPtr plan_insert(const InsertStmt& ins);
        [[nodiscard]] LogicalPlanPtr plan_update(const UpdateStmt& upd);
        [[nodiscard]] LogicalPlanPtr plan_delete(const DeleteStmt& del);

        [[nodiscard]] std::shared_ptr<Table> resolve_table(const std::string& name);

        Catalog& catalog_;
    };

} // namespace corodb::opt
