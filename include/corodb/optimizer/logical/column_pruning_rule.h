// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file column_pruning_rule.h
 * @brief 列裁剪优化规则（R4）。
 *
 * ## 规则说明
 *
 * 通过自顶向下传播"需要的列集合"来确定每个算子实际输出哪些列，
 * 在 Scan 节点上为存储层生成最小列列表，并在 Scan 之上插入 Project
 * 以提前消除不需要的列，从而减少行间传递的数据量。
 *
 * 工作流程（自顶向下传播 + 自底向上删减）：
 * 1. 从顶层算子收集必要列集合（SELECT 列表、WHERE 条件、JOIN ON 条件、ORDER BY）。
 * 2. 向下传播：Join 节点将左/右子树需要的列分别传给左/右子节点。
 * 3. 到达 Scan 节点时，若存储层支持列投影，则将所需列集合写入 ScanNode.columns。
 * 4. 在 Scan 之上（如需要）插入 Project，确保向上返回的 schema 一致。
 *
 * 不适用场景：当需要所有列时（SELECT *、FULL SCAN）不做裁剪。
 *
 * ## SQL 示例
 *
 * **示例 1 — SELECT 列裁剪**
 * ```sql
 * SELECT name FROM employees;
 * ```
 * 表 employees 含 (id, name, dept_id, salary) 四列；
 * 只需 `name` 列，裁剪后 Scan 仅读取该列：
 * ```
 * Project(name)
 *   └─ Scan(employees, cols=[name])   ← 其他三列不再从存储层读取
 * ```
 *
 * **示例 2 — WHERE + SELECT 联合裁剪**
 * ```sql
 * SELECT name FROM employees WHERE dept_id = 10;
 * ```
 * Filter 引用 `dept_id`，SELECT 引用 `name`，合并后 Scan 仅读 (name, dept_id)：
 * ```
 * Project(name)
 *   └─ Filter(dept_id = 10)
 *        └─ Scan(employees, cols=[name, dept_id])
 * ```
 *
 * **示例 3 — Join 两侧独立裁剪**
 * ```sql
 * SELECT e.name, d.name
 *   FROM employees e
 *   JOIN departments d ON e.dept_id = d.id;
 * ```
 * 左侧 Scan(employees) 仅需 (name, dept_id)，右侧 Scan(departments) 仅需 (id, name)：
 * ```
 * Project(e.name, d.name)
 *   └─ Join ON e.dept_id = d.id
 *        ├─ Scan(employees, cols=[name, dept_id])
 *        └─ Scan(departments, cols=[id, name])
 * ```
 *
 * **示例 4 — 聚合裁剪**
 * ```sql
 * SELECT dept_id, COUNT(*) FROM employees GROUP BY dept_id;
 * ```
 * Scan 仅需 `dept_id` 列（COUNT(*) 不引用具体列），其余列全部裁剪。
 */

#pragma once

#include <optional>
#include <string>
#include <unordered_set>

#include "corodb/optimizer/logical/rule.h"

namespace corodb::opt {

    /**
     * @brief 通过传播"需要的列集合"在 Scan 节点上裁剪不必要的列（规则 R4）。
     *
     * 自顶向下收集每个算子所需的列集合，到达 Scan 时将列集合写入节点并在其上
     * 插入最小 Project，减少存储层读取量和行间传递的数据宽度。
     */
    class ColumnPruningRule final : public Rule {
    public:
        std::string name() const override;

        RuleResult apply(LogicalPlanPtr plan) override;

    private:
        using ColSet = std::unordered_set<std::string>;

        LogicalPlanPtr rewrite(LogicalPlanPtr plan, std::optional<ColSet> needed, bool& changed);
    };

} // namespace corodb::opt
