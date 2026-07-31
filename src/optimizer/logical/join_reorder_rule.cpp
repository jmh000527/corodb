// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file join_reorder_rule.cpp
// @brief 小表左置 Join 重排序规则（R5）实现。

#include <algorithm>

#include "corodb/optimizer/logical/join_reorder_rule.h"
#include "corodb/storage/table.h"

namespace corodb::opt {

    namespace detail {

        // 估算子树基数：Scan 用真实行数；Filter 1/3；Aggregate 1/10；
        // **Join 乘积模型**：内联输出基数 = L×R / max(NDV_join_key_left, NDV_join_key_right)。
        // 若无等值 ON 或无 NDV（无索引），回退到保守估算 max(L,R)。
        std::size_t estimate_subtree_size(const LogicalPlan& p) {
            return std::visit(
                    [&](const auto& n) -> std::size_t {
                        using T = std::decay_t<decltype(n)>;
                        if constexpr (std::is_same_v<T, LogicalScan>) {
                            return n.table ? std::max<std::size_t>(n.table->estimated_row_count(), 1) : 1;
                        } else if constexpr (std::is_same_v<T, LogicalJoin>) {
                            std::size_t l = n.left ? estimate_subtree_size(*n.left) : 1;
                            std::size_t r = n.right ? estimate_subtree_size(*n.right) : 1;
                            // 尝试乘积模型：需要等值 ON 条件 + join key NDV。
                            if (n.on.has_value() && n.on->kind == BoolExpr::Kind::Comparison &&
                                n.on->cmp.has_value() && n.on->cmp->op == CompareOp::Eq) {
                                const auto* lk = std::get_if<ColumnRef>(&n.on->cmp->lhs);
                                const auto* rk = std::get_if<ColumnRef>(&n.on->cmp->rhs);
                                if (lk && rk) {
                                    // 从子树的 Scan 取 join key 列的 NDV（需绑定名匹配；任一侧有索引即可）。
                                    auto ndv_from = [](const LogicalPlan& plan, const ColumnRef& col) -> std::size_t {
                                        if (plan.kind != LogicalKind::Scan)
                                            return 0;
                                        const auto& sc = std::get<LogicalScan>(plan.node);
                                        if (!sc.table)
                                            return 0;
                                        const std::string binding = sc.alias.empty() ? sc.table->name() : sc.alias;
                                        if (!col.table.empty() && col.table != binding)
                                            return 0;
                                        return sc.table->index_distinct_count(col.name, 10000);
                                    };
                                    std::size_t ndv = 0;
                                    for (const auto* key: { lk, rk }) {
                                        if (n.left)
                                            ndv = std::max(ndv, ndv_from(*n.left, *key));
                                        if (n.right)
                                            ndv = std::max(ndv, ndv_from(*n.right, *key));
                                    }
                                    if (ndv > 0) {
                                        // 乘积模型：|L| * |R| / ndv（ndv=1 即真·笛卡尔积）
                                        return std::max<std::size_t>((l * r) / ndv, 1);
                                    }
                                }
                            }
                            // 回退：保守取较大侧
                            return std::max<std::size_t>(std::max(l, r), 1);
                        } else if constexpr (std::is_same_v<T, LogicalAggregate>) {
                            std::size_t c = n.child ? estimate_subtree_size(*n.child) : 0;
                            return std::max<std::size_t>(c / 10, 1);
                        } else if constexpr (std::is_same_v<T, LogicalFilter>) {
                            std::size_t c = n.child ? estimate_subtree_size(*n.child) : 0;
                            return std::max<std::size_t>(c / 3, 1);
                        } else if constexpr (requires { n.child; }) {
                            return n.child ? estimate_subtree_size(*n.child) : 0;
                        } else if constexpr (std::is_same_v<T, LogicalDML>) {
                            return n.source ? estimate_subtree_size(*n.source) : 0;
                        } else {
                            return 0;
                        }
                    },
                    p.node);
        }

    } // namespace detail

    /** @brief 返回规则名称标识符。 */
    std::string JoinReorderRule::name() const {
        return "R5.SmallTableLeftJoin";
    }

    /**
     * @brief 对外入口：触发 Join 重排序改写（小表左置）。
     */
    RuleResult JoinReorderRule::apply(LogicalPlanPtr plan) {
        bool changed = false;
        plan = rewrite(std::move(plan), changed);
        return RuleResult{ std::move(plan), changed };
    }

    /**
     * @brief 递归遍历计划树，对 Inner Join 节点按子树大小估算交换左右子树。
     */
    LogicalPlanPtr JoinReorderRule::rewrite(LogicalPlanPtr plan, bool& changed) {
        if (!plan)
            return plan;
        std::visit(
                [&](auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LogicalJoin>) {
                        n.left = rewrite(std::move(n.left), changed);
                        n.right = rewrite(std::move(n.right), changed);
                        if (n.join_type == JoinType::Inner && n.left && n.right) {
                            std::size_t lsz = detail::estimate_subtree_size(*n.left);
                            std::size_t rsz = detail::estimate_subtree_size(*n.right);
                            if (rsz < lsz) {
                                std::swap(n.left, n.right);
                                changed = true;
                            }
                        }
                    } else if constexpr (requires { n.child; }) {
                        n.child = rewrite(std::move(n.child), changed);
                    } else if constexpr (std::is_same_v<T, LogicalDML>) {
                        if (n.source)
                            n.source = rewrite(std::move(n.source), changed);
                    }
                },
                plan->node);
        return plan;
    }

} // namespace corodb::opt
