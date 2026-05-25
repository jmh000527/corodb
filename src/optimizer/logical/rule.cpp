// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file rule.cpp
// @brief 查询优化规则工厂函数及 RuleSet 的实现。

#include "corodb/optimizer/logical/rule.h"

#include "corodb/optimizer/logical/column_pruning_rule.h"
#include "corodb/optimizer/logical/constant_folding_rule.h"
#include "corodb/optimizer/logical/join_reorder_rule.h"
#include "corodb/optimizer/logical/predicate_pushdown_rule.h"
#include "corodb/optimizer/logical/projection_merge_rule.h"

namespace corodb::opt {

    /**
     * @brief 按顺序对所有规则循环应用，直到没有规则再触发改写或达到最大迭代次数。
     * @param plan 输入逻辑计划。
     * @param stats 若非空，接收应用统计信息。
     * @param max_iterations 最大迭代轮次，防止死循环。
     * @return 改写后的逻辑计划。
     */
    LogicalPlanPtr RuleSet::apply(LogicalPlanPtr plan, ApplyStats* stats, int max_iterations) const {
        ApplyStats local;
        for (int iter = 0; iter < max_iterations; ++iter) {
            bool any_changed = false;
            for (const auto& r: rules_) {
                auto result = r->apply(std::move(plan));
                plan = std::move(result.plan);
                if (result.changed) {
                    any_changed = true;
                    ++local.total_rewrites;
                }
            }
            local.iterations = iter + 1;
            if (!any_changed)
                break;
        }
        if (stats)
            *stats = local;
        return plan;
    }

    /**
     * @brief 构造包含所有默认优化规则的 RuleSet（常量折叠、谓词下推、列裁剪、投影合并、Join 重排序）。
     */
    RuleSet make_default_rules() {
        RuleSet rs;
        rs.add(std::make_unique<ConstantFoldingRule>());
        rs.add(std::make_unique<PredicatePushdownRule>());
        rs.add(std::make_unique<ColumnPruningRule>());
        rs.add(std::make_unique<ProjectionMergeRule>());
        rs.add(std::make_unique<JoinReorderRule>());
        return rs;
    }

    /**
     * @brief 创建常量折叠规则实例（R3）。
     */
    RulePtr make_constant_folding_rule() {
        return std::make_unique<ConstantFoldingRule>();
    }
    /**
     * @brief 创建谓词下推规则实例（R1）。
     */
    RulePtr make_predicate_pushdown_rule() {
        return std::make_unique<PredicatePushdownRule>();
    }
    /**
     * @brief 创建列裁剪规则实例（R4）。
     */
    RulePtr make_column_pruning_rule() {
        return std::make_unique<ColumnPruningRule>();
    }
    /**
     * @brief 创建相邻投影合并规则实例（R2）。
     */
    RulePtr make_projection_merge_rule() {
        return std::make_unique<ProjectionMergeRule>();
    }
    /**
     * @brief 创建小表左置 Join 重排序规则实例（R5）。
     */
    RulePtr make_join_reorder_rule() {
        return std::make_unique<JoinReorderRule>();
    }

} // namespace corodb::opt
