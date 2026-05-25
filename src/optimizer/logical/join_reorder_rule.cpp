// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file join_reorder_rule.cpp
// @brief 小表左置 Join 重排序规则（R5）实现。

#include <algorithm>

#include "corodb/optimizer/logical/join_reorder_rule.h"

namespace corodb::opt {

    namespace detail {

        // 估算子树的相对大小（以节点数量为代理指标）
        std::size_t estimate_subtree_size(const LogicalPlan& p) {
            return std::visit(
                    [&](const auto& n) -> std::size_t {
                        using T = std::decay_t<decltype(n)>;
                        if constexpr (std::is_same_v<T, LogicalScan>) {
                            return 1;
                        } else if constexpr (std::is_same_v<T, LogicalJoin>) {
                            std::size_t l = n.left ? estimate_subtree_size(*n.left) : 0;
                            std::size_t r = n.right ? estimate_subtree_size(*n.right) : 0;
                            return std::max<std::size_t>(l, r) + 1;
                        } else if constexpr (std::is_same_v<T, LogicalAggregate>) {
                            std::size_t c = n.child ? estimate_subtree_size(*n.child) : 0;
                            return (c + 1) / 2;
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
