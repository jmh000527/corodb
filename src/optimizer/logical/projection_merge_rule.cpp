// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file projection_merge_rule.cpp
// @brief 相邻 Project 合并优化规则（R2）实现。

#include "corodb/optimizer/logical/projection_merge_rule.h"

namespace corodb::opt {

    /** @brief 返回规则名称标识符。 */
    std::string ProjectionMergeRule::name() const {
        return "R2.ProjectionMerge";
    }

    /**
     * @brief 对外入口：触发相邻 Project 合并改写。
     */
    RuleResult ProjectionMergeRule::apply(LogicalPlanPtr plan) {
        bool changed = false;
        plan = rewrite(std::move(plan), changed);
        return RuleResult{ std::move(plan), changed };
    }

    /**
     * @brief 递归遍历计划树；若外层 Project 的每列均为简单 ColumnRef 且内层也是 Project，
     *        则将两层合并为单层，减少运行时投影开销。
     */
    LogicalPlanPtr ProjectionMergeRule::rewrite(LogicalPlanPtr plan, bool& changed) {
        if (!plan)
            return plan;
        std::visit(
                [&](auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LogicalJoin>) {
                        n.left = rewrite(std::move(n.left), changed);
                        n.right = rewrite(std::move(n.right), changed);
                    } else if constexpr (requires { n.child; }) {
                        n.child = rewrite(std::move(n.child), changed);
                    } else if constexpr (std::is_same_v<T, LogicalDML>) {
                        if (n.source)
                            n.source = rewrite(std::move(n.source), changed);
                    }
                },
                plan->node);

        if (plan->kind != LogicalKind::Project)
            return plan;
        auto& outer = std::get<LogicalProject>(plan->node);
        if (!outer.child || outer.child->kind != LogicalKind::Project)
            return plan;

        // 外层每列必须是无表名前缀的纯 ColumnRef，否则放弃合并
        for (const auto& col: outer.columns) {
            auto* cr = std::get_if<ColumnRef>(&col.expr);
            if (!cr || !cr->table.empty())
                return plan;
        }
        auto& inner = std::get<LogicalProject>(outer.child->node);

        // 用内层对应列的表达式替换外层列，保留外层的 output_name
        std::vector<LogicalColumn> merged;
        merged.reserve(outer.columns.size());
        for (auto& ocol: outer.columns) {
            auto* cr = std::get_if<ColumnRef>(&ocol.expr);
            bool found = false;
            for (auto& icol: inner.columns) {
                if (icol.output_name == cr->name) {
                    LogicalColumn nc;
                    nc.expr = icol.expr;
                    nc.output_name = ocol.output_name;
                    merged.push_back(std::move(nc));
                    found = true;
                    break;
                }
            }
            if (!found)
                return plan;
        }

        LogicalProject newp;
        newp.columns = std::move(merged);
        newp.child = std::move(inner.child);
        plan->node = std::move(newp);
        changed = true;
        return plan;
    }

} // namespace corodb::opt
