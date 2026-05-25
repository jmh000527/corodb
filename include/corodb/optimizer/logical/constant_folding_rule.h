// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file constant_folding_rule.h
 * @brief 常量折叠优化规则（R3）。
 *
 * ## 规则说明
 *
 * 在编译期（查询规划阶段）对只包含字面量的算术或逻辑表达式进行求值，
 * 将结果替换为单个常量节点，从而消除执行期的重复运算。
 *
 * 支持的折叠类型：
 * - **算术表达式**：`+` `-` `*` `/` `%`，操作数均为 IntLiteral 或 FloatLiteral。
 * - **字符串拼接**：`||` 两侧均为 StringLiteral。
 * - **比较表达式**：两侧均为相同类型的字面量（求值为 BoolLiteral）。
 * - **逻辑表达式**：AND/OR/NOT 在操作数全为 BoolLiteral 时直接短路求值。
 * - **类型转换**：对字面量执行的 CAST 可在规划期完成。
 *
 * 不折叠：任何一侧包含列引用、子查询、聚合函数等运行期才能确定的表达式。
 *
 * ## SQL 示例
 *
 * **示例 1 — 算术表达式折叠**
 * ```sql
 * SELECT * FROM employees WHERE salary > 50000 + 30000;
 * ```
 * 优化前（谓词中含算术节点）：
 * ```
 * Filter(salary > (50000 + 30000))
 *   └─ Scan(employees)
 * ```
 * 优化后：
 * ```
 * Filter(salary > 80000)     ← 50000+30000 在规划期求值
 *   └─ Scan(employees)
 * ```
 *
 * **示例 2 — 复合算术折叠**
 * ```sql
 * UPDATE products SET price = price * (1.0 - 0.1 * 2);
 * ```
 * `1.0 - 0.1 * 2` 完全由字面量组成，折叠为 `0.8`。
 *
 * **示例 3 — 比较折叠（恒真/恒假谓词）**
 * ```sql
 * SELECT * FROM orders WHERE 1 = 1;
 * SELECT * FROM orders WHERE 0 > 1;
 * ```
 * 第一个 `1 = 1` 折叠为 `TRUE`，Filter 可进一步被消除；
 * 第二个 `0 > 1` 折叠为 `FALSE`，Filter 可替换为 EmptyScan。
 *
 * **示例 4 — 逻辑表达式短路**
 * ```sql
 * SELECT * FROM t WHERE TRUE AND status = 'active';
 * ```
 * `TRUE AND status = 'active'` 折叠为 `status = 'active'`。
 *
 * **示例 5 — 字符串拼接折叠**
 * ```sql
 * SELECT * FROM t WHERE category = 'elec' || 'tronics';
 * ```
 * `'elec' || 'tronics'` 折叠为 `'electronics'`。
 */

#pragma once

#include <type_traits>

#include "corodb/optimizer/logical/rule.h"

namespace corodb::opt {

    namespace detail {
        Expression fold_expression(Expression expr);
        BoolExpr fold_bool(BoolExpr expr);
    } // namespace detail

    /**
     * @brief 对只含字面量的表达式在规划期求值，替换为常量（规则 R3）。
     *
     * 递归遍历所有 Filter 和 Project 节点的表达式树；对纯字面量子树进行
     * 算术 / 比较 / 逻辑求值并折叠为单个常量节点。
     */
    class ConstantFoldingRule final : public Rule {
    public:
        std::string name() const override;

        RuleResult apply(LogicalPlanPtr plan) override;

    private:
        void rewrite(LogicalPlanPtr& plan, bool& changed);

        template<typename T>
        void rewrite_node(T& n, bool& changed) {
            if constexpr (std::is_same_v<T, LogicalFilter>) {
                auto before = serialize(n.predicate);
                n.predicate = detail::fold_bool(std::move(n.predicate));
                if (serialize(n.predicate) != before)
                    changed = true;
                rewrite(n.child, changed);
            } else if constexpr (std::is_same_v<T, LogicalProject>) {
                for (auto& col: n.columns) {
                    auto before = serialize_expr(col.expr);
                    col.expr = detail::fold_expression(std::move(col.expr));
                    if (serialize_expr(col.expr) != before)
                        changed = true;
                }
                rewrite(n.child, changed);
            } else if constexpr (std::is_same_v<T, LogicalJoin>) {
                if (n.on.has_value()) {
                    auto before = serialize(*n.on);
                    *n.on = detail::fold_bool(std::move(*n.on));
                    if (serialize(*n.on) != before)
                        changed = true;
                }
                rewrite(n.left, changed);
                rewrite(n.right, changed);
            } else if constexpr (std::is_same_v<T, LogicalAggregate>) {
                if (n.having.has_value()) {
                    auto before = serialize(*n.having);
                    *n.having = detail::fold_bool(std::move(*n.having));
                    if (serialize(*n.having) != before)
                        changed = true;
                }
                rewrite(n.child, changed);
            } else if constexpr (std::is_same_v<T, LogicalSort>) {
                for (auto& key: n.keys) {
                    auto before = serialize_expr(key.expr);
                    key.expr = detail::fold_expression(std::move(key.expr));
                    if (serialize_expr(key.expr) != before)
                        changed = true;
                }
                rewrite(n.child, changed);
            } else if constexpr (std::is_same_v<T, LogicalLimit>) {
                rewrite(n.child, changed);
            } else if constexpr (std::is_same_v<T, LogicalValues>) {
                for (auto& row: n.rows) {
                    for (auto& e: row) {
                        auto before = serialize_expr(e);
                        e = detail::fold_expression(std::move(e));
                        if (serialize_expr(e) != before)
                            changed = true;
                    }
                }
            } else if constexpr (std::is_same_v<T, LogicalDML>) {
                for (auto& e: n.set_exprs) {
                    auto before = serialize_expr(e);
                    e = detail::fold_expression(std::move(e));
                    if (serialize_expr(e) != before)
                        changed = true;
                }
                if (n.where.has_value()) {
                    auto before = serialize(*n.where);
                    *n.where = detail::fold_bool(std::move(*n.where));
                    if (serialize(*n.where) != before)
                        changed = true;
                }
                if (n.source)
                    rewrite(n.source, changed);
            }
            // LogicalScan, LogicalDDL: no expressions to fold
        }

        std::string serialize_expr(const Expression& e);
        std::string serialize(const BoolExpr& b);
        std::string value_str(const Value& v);
    };

} // namespace corodb::opt
