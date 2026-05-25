// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file rule.h @brief 逻辑规则优化框架。 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "corodb/plan/logical_plan.h"

namespace corodb::opt {

    /** @brief 单条规则应用后的结果。 */
    struct RuleResult {
        LogicalPlanPtr plan;
        bool changed{ false };
    };

    /** @brief 逻辑优化规则接口。 */
    class Rule {
    public:
        virtual ~Rule() = default;

        /** @brief 返回规则名称。 */
        [[nodiscard]] virtual std::string name() const = 0;

        /** @brief 对整棵计划树应用本规则一次。 */
        [[nodiscard]] virtual RuleResult apply(LogicalPlanPtr plan) = 0;
    };

    using RulePtr = std::unique_ptr<Rule>;

    /** @brief 按固定点策略反复应用规则集合。 */
    class RuleSet {
    public:
        RuleSet() = default;
        explicit RuleSet(std::vector<RulePtr> rules) : rules_(std::move(rules)) {
        }

        void add(RulePtr rule) {
            rules_.push_back(std::move(rule));
        }

        /** @brief 规则集合的迭代执行统计。 */
        struct ApplyStats {
            int iterations{ 0 };
            int total_rewrites{ 0 };
        };

        /** @brief 按固定点策略对计划树应用本规则集，返回优化后的计划。 */
        LogicalPlanPtr apply(LogicalPlanPtr plan, ApplyStats* stats = nullptr, int max_iterations = 16) const;

        [[nodiscard]] std::size_t size() const noexcept {
            return rules_.size();
        }

    private:
        std::vector<RulePtr> rules_;
    };

    /** @brief 返回默认规则集。 */
    [[nodiscard]] RuleSet make_default_rules();

    /** @brief 返回单条规则实例。 */
    [[nodiscard]] RulePtr make_constant_folding_rule();
    [[nodiscard]] RulePtr make_predicate_pushdown_rule();
    [[nodiscard]] RulePtr make_column_pruning_rule();
    [[nodiscard]] RulePtr make_projection_merge_rule();
    [[nodiscard]] RulePtr make_join_reorder_rule();

} // namespace corodb::opt
