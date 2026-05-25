// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file predicate_pushdown_rule.h
 * @brief 谓词下推优化规则（R1）。
 *
 * ## 规则说明
 *
 * 将 Filter（WHERE/ON 谓词）尽量下推至靠近数据源的位置，使扫描时即可过滤行，
 * 避免将大量不必要的行传递给上层算子（Join、Project 等）。
 *
 * - 对 **Inner Join**：将只引用左表的合取项下推到左子树，只引用右表的下推到
 *   右子树，同时引用两表的合取项留在 Join 的 ON 条件中。
 * - 对 **Project / Sort**：直接穿透，Filter 移至投影/排序的子节点之上。
 * - 对其他节点：保守保留，不移动。
 *
 * ## SQL 示例
 *
 * **示例 1 — 谓词穿透 Join 下推到左子树**
 * ```sql
 * SELECT e.name, d.name
 *   FROM employees e
 *   JOIN departments d ON e.dept_id = d.id
 *  WHERE e.salary > 80000;
 * ```
 * 优化前（逻辑计划）：
 * ```
 * Filter(e.salary > 80000)
 *   └─ Join ON e.dept_id = d.id
 *        ├─ Scan(employees)
 *        └─ Scan(departments)
 * ```
 * 优化后：
 * ```
 * Join ON e.dept_id = d.id
 *   ├─ Filter(e.salary > 80000)
 *   │    └─ Scan(employees)      ← 过滤后仅剩少量行
 *   └─ Scan(departments)
 * ```
 *
 * **示例 2 — 谓词穿透 Project 下推**
 * ```sql
 * SELECT name FROM (
 *   SELECT id, name, salary FROM employees WHERE dept_id = 10
 * ) t
 * WHERE salary > 50000;
 * ```
 * 内层子查询的 `salary > 50000` 会穿透外层 Project 下推到 Scan 之上。
 *
 * **示例 3 — 两表谓词分发**
 * ```sql
 * SELECT *
 *   FROM orders o
 *   JOIN products p ON o.product_id = p.id
 *  WHERE o.amount > 100 AND p.price < 500;
 * ```
 * `o.amount > 100` 下推到 orders 侧，`p.price < 500` 下推到 products 侧。
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "corodb/optimizer/logical/rule.h"

namespace corodb::opt {

    namespace detail {

        using StringSet = std::unordered_set<std::string>;

        /** @brief 收集表达式中所有列引用的表名/别名。 */
        void collect_expr_tables(const Expression& e, StringSet& out);

        /** @brief 收集布尔表达式中所有列引用的表名/别名。 */
        void collect_bool_tables(const BoolExpr& b, StringSet& out);

        /** @brief 收集计划子树中涉及的所有基表名和别名。 */
        void collect_plan_tables(const LogicalPlan& p, StringSet& out);

        /** @brief 将顶层 AND 合取拆分为独立的 BoolExpr 列表。 */
        void split_and(BoolExpr expr, std::vector<BoolExpr>& out);

        /** @brief 将合取项列表重新合并为单个 AND BoolExpr（列表不可为空）。 */
        BoolExpr combine_and(std::vector<BoolExpr> conjuncts);

        /** @brief 返回 refs ⊆ owned 是否成立。 */
        bool refs_subset_of(const StringSet& refs, const StringSet& owned);

        /** @brief 在 child 之上包裹一个 Filter（若 child 已是 Filter 则合并谓词）。 */
        LogicalPlanPtr wrap_filter(std::vector<BoolExpr> conjuncts, LogicalPlanPtr child);

    } // namespace detail

    /**
     * @brief 将 Filter 谓词下推至尽可能靠近数据源的位置（规则 R1）。
     *
     * 对 Inner Join 按引用的表名将合取项分发到左右子树；
     * 对 Project/Sort 直接穿透；对其他节点保守保留。
     */
    class PredicatePushdownRule final : public Rule {
    public:
        std::string name() const override;

        RuleResult apply(LogicalPlanPtr plan) override;

    private:
        LogicalPlanPtr rewrite(LogicalPlanPtr plan, bool& changed);
        LogicalPlanPtr push_through(LogicalPlanPtr child, std::vector<BoolExpr>& conjuncts,
                                    std::vector<BoolExpr>& stay_here, bool& changed);
    };

} // namespace corodb::opt
