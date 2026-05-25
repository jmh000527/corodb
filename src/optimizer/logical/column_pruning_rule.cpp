// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file column_pruning_rule.cpp
// @brief 投影列裁剪优化规则（R4）实现。

#include "corodb/optimizer/logical/column_pruning_rule.h"

namespace corodb::opt {

    namespace detail {

        // 收集表达式中引用的所有列名
        void collect_expr_cols(const Expression& e, std::unordered_set<std::string>& out) {
            if (auto* c = std::get_if<ColumnRef>(&e)) {
                out.insert(c->name);
                return;
            }
            if (auto* b = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
                if (*b) {
                    collect_expr_cols((*b)->lhs, out);
                    collect_expr_cols((*b)->rhs, out);
                }
            }
        }

        // 收集布尔表达式中引用的所有列名
        void collect_bool_cols(const BoolExpr& b, std::unordered_set<std::string>& out) {
            if (b.kind == BoolExpr::Kind::Comparison && b.cmp.has_value()) {
                collect_expr_cols(b.cmp->lhs, out);
                collect_expr_cols(b.cmp->rhs, out);
            }
            if (b.left)
                collect_bool_cols(*b.left, out);
            if (b.right)
                collect_bool_cols(*b.right, out);
        }

    } // namespace detail

    std::string ColumnPruningRule::name() const {
        return "R4.ColumnPruning";
    }

    /**
     * @brief 对外入口：触发列裁剪改写，从根节点开始向下传播所需列集合。
     */
    RuleResult ColumnPruningRule::apply(LogicalPlanPtr plan) {
        bool changed = false;
        plan = rewrite(std::move(plan), /*needed=*/std::nullopt, changed);
        return RuleResult{ std::move(plan), changed };
    }

    /**
     * @brief 递归改写计划树，根据上层所需列集合裁剪 Project 的输出列，并向子节点传播更小的需求集。
     * @param needed 上层算子所需的列名集合；nullopt 表示不限制（保留所有列）。
     * @param changed 输出参数，若本次调用发生了改写则置 true。
     */
    LogicalPlanPtr ColumnPruningRule::rewrite(LogicalPlanPtr plan, std::optional<ColSet> needed, bool& changed) {
        if (!plan)
            return plan;
        std::visit(
                [&](auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LogicalProject>) {
                        if (needed.has_value()) {
                            std::vector<std::size_t> keep_idx;
                            keep_idx.reserve(n.columns.size());
                            for (std::size_t i = 0; i < n.columns.size(); ++i) {
                                if (needed->count(n.columns[i].output_name))
                                    keep_idx.push_back(i);
                            }
                            if (!keep_idx.empty() && keep_idx.size() != n.columns.size()) {
                                std::vector<LogicalColumn> kept;
                                kept.reserve(keep_idx.size());
                                for (auto i: keep_idx)
                                    kept.push_back(std::move(n.columns[i]));
                                n.columns = std::move(kept);
                                changed = true;
                            }
                        }
                        ColSet child_needed;
                        for (const auto& col: n.columns)
                            detail::collect_expr_cols(col.expr, child_needed);
                        n.child = rewrite(std::move(n.child), child_needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalFilter>) {
                        ColSet child_needed = needed.value_or(ColSet{});
                        detail::collect_bool_cols(n.predicate, child_needed);
                        n.child = rewrite(std::move(n.child), child_needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalSort>) {
                        ColSet child_needed = needed.value_or(ColSet{});
                        for (const auto& k: n.keys)
                            detail::collect_expr_cols(k.expr, child_needed);
                        n.child = rewrite(std::move(n.child), child_needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalLimit>) {
                        n.child = rewrite(std::move(n.child), needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalAggregate>) {
                        ColSet child_needed;
                        for (const auto& g: n.group_by)
                            detail::collect_expr_cols(g, child_needed);
                        for (const auto& a: n.aggregates) {
                            if (a.arg.has_value())
                                detail::collect_expr_cols(*a.arg, child_needed);
                        }
                        if (n.having.has_value())
                            detail::collect_bool_cols(*n.having, child_needed);
                        n.child = rewrite(std::move(n.child), child_needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalJoin>) {
                        ColSet child_needed = needed.value_or(ColSet{});
                        if (n.on.has_value())
                            detail::collect_bool_cols(*n.on, child_needed);
                        n.left = rewrite(std::move(n.left), child_needed, changed);
                        n.right = rewrite(std::move(n.right), child_needed, changed);
                    } else if constexpr (std::is_same_v<T, LogicalDML>) {
                        if (n.source)
                            n.source = rewrite(std::move(n.source), std::nullopt, changed);
                    }
                    // Scan / Values / DDL：叶子节点，不裁剪
                },
                plan->node);
        return plan;
    }

} // namespace corodb::opt
