// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file predicate_pushdown_rule.cpp
// @brief 谓词下推优化规则（R1）实现。

#include "corodb/optimizer/logical/predicate_pushdown_rule.h"

namespace corodb::opt {

    namespace detail {

        // 收集表达式中引用的表名集合
        void collect_expr_tables(const Expression& e, StringSet& out) {
            if (auto* c = std::get_if<ColumnRef>(&e)) {
                if (!c->table.empty())
                    out.insert(c->table);
                return;
            }
            if (auto* b = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
                if (*b) {
                    collect_expr_tables((*b)->lhs, out);
                    collect_expr_tables((*b)->rhs, out);
                }
            }
        }

        // 收集布尔表达式中引用的表名集合
        void collect_bool_tables(const BoolExpr& b, StringSet& out) {
            if (b.cmp.has_value()) {
                collect_expr_tables(b.cmp->lhs, out);
                collect_expr_tables(b.cmp->rhs, out);
            }
            if (b.left)
                collect_bool_tables(*b.left, out);
            if (b.right)
                collect_bool_tables(*b.right, out);
        }

        // 收集计划子树中涉及的所有表名（含别名）
        void collect_plan_tables(const LogicalPlan& p, StringSet& out) {
            std::visit(
                    [&](const auto& n) {
                        using T = std::decay_t<decltype(n)>;
                        if constexpr (std::is_same_v<T, LogicalScan>) {
                            if (n.table)
                                out.insert(n.table->name());
                            if (!n.alias.empty())
                                out.insert(n.alias);
                        } else if constexpr (std::is_same_v<T, LogicalJoin>) {
                            if (n.left)
                                collect_plan_tables(*n.left, out);
                            if (n.right)
                                collect_plan_tables(*n.right, out);
                        } else if constexpr (requires { n.child; }) {
                            if (n.child)
                                collect_plan_tables(*n.child, out);
                        } else if constexpr (std::is_same_v<T, LogicalDML>) {
                            if (n.table)
                                out.insert(n.table->name());
                            if (n.source)
                                collect_plan_tables(*n.source, out);
                        }
                    },
                    p.node);
        }

        // 将 AND 连接的布尔表达式拆解为独立合取项列表
        void split_and(BoolExpr expr, std::vector<BoolExpr>& out) {
            if (expr.kind == BoolExpr::Kind::And && expr.left && expr.right) {
                split_and(std::move(*expr.left), out);
                split_and(std::move(*expr.right), out);
                return;
            }
            out.push_back(std::move(expr));
        }

        // 将合取项列表重新组合为左结合的 AND 表达式树
        BoolExpr combine_and(std::vector<BoolExpr> conjuncts) {
            BoolExpr cur = std::move(conjuncts.front());
            for (std::size_t i = 1; i < conjuncts.size(); ++i) {
                BoolExpr combo;
                combo.kind = BoolExpr::Kind::And;
                combo.left = std::make_unique<BoolExpr>(std::move(cur));
                combo.right = std::make_unique<BoolExpr>(std::move(conjuncts[i]));
                cur = std::move(combo);
            }
            return cur;
        }

        // 判断 refs 中的所有表名是否均包含于 owned 集合中
        bool refs_subset_of(const StringSet& refs, const StringSet& owned) {
            for (const auto& r: refs) {
                if (owned.find(r) == owned.end())
                    return false;
            }
            return true;
        }

        // 将合取项包装为 Filter 节点（若 child 已是 Filter 则合并到其谓词中）
        LogicalPlanPtr wrap_filter(std::vector<BoolExpr> conjuncts, LogicalPlanPtr child) {
            if (conjuncts.empty())
                return child;
            if (child && child->kind == LogicalKind::Filter) {
                auto& f = std::get<LogicalFilter>(child->node);
                std::vector<BoolExpr> existing;
                split_and(std::move(f.predicate), existing);
                for (auto& c: conjuncts)
                    existing.push_back(std::move(c));
                f.predicate = combine_and(std::move(existing));
                return child;
            }
            return LogicalPlan::make_filter(combine_and(std::move(conjuncts)), std::move(child));
        }

    } // namespace detail

    std::string PredicatePushdownRule::name() const {
        return "R1.PredicatePushdown";
    }

    /**
     * @brief 对外入口：触发谓词下推改写。
     */
    RuleResult PredicatePushdownRule::apply(LogicalPlanPtr plan) {
        bool changed = false;
        plan = rewrite(std::move(plan), changed);
        return RuleResult{ std::move(plan), changed };
    }

    /**
     * @brief 递归遍历计划树，对 Filter 节点尝试将合取项下推到子节点。
     */
    LogicalPlanPtr PredicatePushdownRule::rewrite(LogicalPlanPtr plan, bool& changed) {
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

        if (plan->kind != LogicalKind::Filter)
            return plan;
        auto& filter = std::get<LogicalFilter>(plan->node);
        if (!filter.child)
            return plan;

        std::vector<BoolExpr> conjuncts;
        detail::split_and(std::move(filter.predicate), conjuncts);
        LogicalPlanPtr child = std::move(filter.child);
        std::vector<BoolExpr> stay_here;
        bool local_changed = false;
        child = push_through(std::move(child), conjuncts, stay_here, local_changed);
        if (local_changed)
            changed = true;
        if (stay_here.empty())
            return child;
        return detail::wrap_filter(std::move(stay_here), std::move(child));
    }

    /**
     * @brief 将合取项列表尝试推入 child 节点（穿越 Project / Sort，分发到 Join 两侧或合并到相邻 Filter）。
     * @param child 目标子节点。
     * @param conjuncts 待下推的合取项（函数返回后可能已清空）。
     * @param stay_here 无法继续下推、需留在当前层的合取项输出。
     * @param changed 若发生改写则置 true。
     * @return 改写后的子节点。
     */
    LogicalPlanPtr PredicatePushdownRule::push_through(LogicalPlanPtr child, std::vector<BoolExpr>& conjuncts,
                                                       std::vector<BoolExpr>& stay_here, bool& changed) {
        if (!child || conjuncts.empty()) {
            for (auto& c: conjuncts)
                stay_here.push_back(std::move(c));
            conjuncts.clear();
            return child;
        }

        // 谓词穿越 Project：将谓词下推到 Project 子节点
        if (child->kind == LogicalKind::Project) {
            auto& proj = std::get<LogicalProject>(child->node);
            std::vector<BoolExpr> still;
            proj.child = push_through(std::move(proj.child), conjuncts, still, changed);
            if (!still.empty()) {
                proj.child = detail::wrap_filter(std::move(still), std::move(proj.child));
                changed = true;
            } else if (!conjuncts.empty()) {
                changed = true;
            }
            conjuncts.clear();
            return child;
        }

        // 谓词穿越 Sort 节点（不影响行排序）
        if (child->kind == LogicalKind::Sort) {
            auto& s = std::get<LogicalSort>(child->node);
            std::vector<BoolExpr> still;
            s.child = push_through(std::move(s.child), conjuncts, still, changed);
            if (!still.empty()) {
                s.child = detail::wrap_filter(std::move(still), std::move(s.child));
            } else {
                changed = true;
            }
            conjuncts.clear();
            return child;
        }

        // Inner Join：按引用表集合将谓词分发到左/右子树
        if (child->kind == LogicalKind::Join) {
            auto& j = std::get<LogicalJoin>(child->node);
            if (j.join_type == JoinType::Inner) {
                // 收集左/右子树所覆盖的表名
                detail::StringSet left_tables, right_tables;
                if (j.left)
                    detail::collect_plan_tables(*j.left, left_tables);
                if (j.right)
                    detail::collect_plan_tables(*j.right, right_tables);

                std::vector<BoolExpr> push_left, push_right;
                for (auto& c: conjuncts) {
                    detail::StringSet refs;
                    detail::collect_bool_tables(c, refs);
                    if (refs.empty()) {
                        stay_here.push_back(std::move(c));
                    } else if (detail::refs_subset_of(refs, left_tables)) {
                        push_left.push_back(std::move(c));
                    } else if (detail::refs_subset_of(refs, right_tables)) {
                        push_right.push_back(std::move(c));
                    } else {
                        stay_here.push_back(std::move(c));
                    }
                }
                conjuncts.clear();

                if (!push_left.empty()) {
                    std::vector<BoolExpr> still_l;
                    j.left = push_through(std::move(j.left), push_left, still_l, changed);
                    if (!still_l.empty())
                        j.left = detail::wrap_filter(std::move(still_l), std::move(j.left));
                    changed = true;
                }
                if (!push_right.empty()) {
                    std::vector<BoolExpr> still_r;
                    j.right = push_through(std::move(j.right), push_right, still_r, changed);
                    if (!still_r.empty())
                        j.right = detail::wrap_filter(std::move(still_r), std::move(j.right));
                    changed = true;
                }
                return child;
            }
            // 非 INNER JOIN 不能下推（Outer Join 语义改变）
            for (auto& c: conjuncts)
                stay_here.push_back(std::move(c));
            conjuncts.clear();
            return child;
        }

        // 相邻 Filter 合并：把新谓词追加到已有 Filter 的 AND 链中
        if (child->kind == LogicalKind::Filter) {
            auto& f = std::get<LogicalFilter>(child->node);
            std::vector<BoolExpr> existing;
            detail::split_and(std::move(f.predicate), existing);
            for (auto& c: conjuncts)
                existing.push_back(std::move(c));
            f.predicate = detail::combine_and(std::move(existing));
            conjuncts.clear();
            changed = true;
            return child;
        }

        for (auto& c: conjuncts)
            stay_here.push_back(std::move(c));
        conjuncts.clear();
        return child;
    }

} // namespace corodb::opt
