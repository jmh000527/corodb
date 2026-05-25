// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file projection_merge_rule.h
 * @brief 相邻 Project 合并优化规则（R2）。
 *
 * ## 规则说明
 *
 * 当逻辑计划树中出现两层嵌套的 Project 节点时，若外层 Project 的所有列均为
 * 纯 ColumnRef（无表名前缀限定、无表达式计算、无重命名），则可将外层 Project
 * 直接重写为内层 Project 对应列的子集，消除一层冗余投影。
 *
 * 该规则通常在 R4（列裁剪）之后触发：列裁剪会为内层算子插入新的 Project，
 * 与原有的外层 Project 形成双层嵌套，由本规则负责合并。
 *
 * ## SQL 示例
 *
 * **示例 1 — 子查询产生的双层 Project**
 * ```sql
 * SELECT id, name
 *   FROM (SELECT id, name, dept_id, salary FROM employees) t;
 * ```
 * 优化前：
 * ```
 * Project(id, name)
 *   └─ Project(id, name, dept_id, salary)
 *        └─ Scan(employees)
 * ```
 * 优化后（合并为单层）：
 * ```
 * Project(id, name)
 *   └─ Scan(employees)
 * ```
 *
 * **示例 2 — 列裁剪（R4）后触发合并**
 * ```sql
 * SELECT name FROM employees WHERE dept_id = 10;
 * ```
 * R4 在 Scan 上方插入 `Project(name, dept_id)` 后，原有的 `Project(name)` 与
 * 之形成双层嵌套，R2 将其合并为 `Project(name)` 直接放在 Scan 上方。
 *
 * **示例 3 — 含别名时不触发合并**
 * ```sql
 * SELECT id AS employee_id, name FROM employees;
 * ```
 * 外层 Project 中 `id AS employee_id` 含重命名，不满足纯 ColumnRef 条件，
 * 规则不做合并（保守处理）。
 */

#pragma once

#include "corodb/optimizer/logical/rule.h"

namespace corodb::opt {

    /**
     * @brief 将相邻两个 Project 节点合并为一个（规则 R2）。
     *
     * 当外层 Project 的所有列均为纯 ColumnRef（无表名前缀、无重命名表达式）时，
     * 直接将其重写为内层 Project 对应列的子集，消除冗余投影层。
     * 通常由 R4（投影裁剪）触发后出现的冗余 Project 嵌套形态。
     */
    class ProjectionMergeRule final : public Rule {
    public:
        std::string name() const override;

        RuleResult apply(LogicalPlanPtr plan) override;

    private:
        LogicalPlanPtr rewrite(LogicalPlanPtr plan, bool& changed);
    };

} // namespace corodb::opt
