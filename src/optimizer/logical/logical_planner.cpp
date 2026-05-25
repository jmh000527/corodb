// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file logical_planner.cpp
// @brief 逻辑查询计划生成器的实现。

#include "corodb/optimizer/logical/logical_planner.h"

#include <stdexcept>
#include <utility>

namespace corodb::opt {

    namespace {
        // 将 SelectItem::value 中的 AggregateExpr 包成 LogicalAggregateItem
        LogicalAggregateItem to_agg_item(const AggregateExpr& agg, std::string out_name) {
            LogicalAggregateItem item;
            item.func = agg.func;
            if (agg.arg.has_value())
                item.arg = Expression{ *agg.arg };
            item.distinct = false;
            item.output_name = std::move(out_name);
            return item;
        }

        // 提取投影项中的表达式（包装：聚合 → AggregateExpr 表达式；普通 → Expression）
        Expression item_to_expr(const SelectStmt::SelectItem& it) {
            if (auto* e = std::get_if<Expression>(&it.value))
                return *e;
            return std::get<AggregateExpr>(it.value);
        }
    } // namespace

    /**
     * @brief 按表名从 Catalog 解析表对象，不存在时抛出异常。
     * @param name 表名。
     * @return 表的 shared_ptr。
     */
    std::shared_ptr<Table> LogicalPlanner::resolve_table(const std::string& name) {
        auto t = catalog_.lookup(name);
        if (!t)
            throw std::runtime_error("[LogicalPlanner] Unknown table: " + name);
        return t;
    }

    /**
     * @brief 将 AST Statement 翻译为逻辑计划树（DDL 直通保留为 LogicalDDL 节点）。
     * @param stmt 已解析的 SQL 语句。
     * @return 逻辑计划根节点。
     */
    LogicalPlanPtr LogicalPlanner::plan(const Statement& stmt) {
        if (auto* s = std::get_if<SelectStmt>(&stmt))
            return plan_select(*s);
        if (auto* s = std::get_if<InsertStmt>(&stmt))
            return plan_insert(*s);
        if (auto* s = std::get_if<UpdateStmt>(&stmt))
            return plan_update(*s);
        if (auto* s = std::get_if<DeleteStmt>(&stmt))
            return plan_delete(*s);
        // 其它语句作为 DDL 直通保留 AST，物理层走旧路径
        return LogicalPlan::make(LogicalKind::DDL, LogicalDDL{ stmt });
    }

    /**
     * @brief 将 SELECT 语句翻译为逻辑计划（Scan → Filter → Aggregate → Sort → Limit → Project）。
     */
    LogicalPlanPtr LogicalPlanner::plan_select(const SelectStmt& select) {
        // 1) FROM + JOINs → 左结合 Join 树
        auto base_table = resolve_table(select.from_table);
        std::string base_alias = select.from_alias.value_or(std::string{});
        LogicalPlanPtr cur = LogicalPlan::make_scan(base_table, base_alias);

        for (const auto& j: select.joins) {
            auto rt = resolve_table(j.table);
            std::string r_alias = j.alias.value_or(std::string{});
            auto right = LogicalPlan::make_scan(rt, std::move(r_alias));

            LogicalJoin lj;
            lj.join_type = j.type;
            lj.on = j.on;
            lj.left = std::move(cur);
            lj.right = std::move(right);
            cur = LogicalPlan::make(LogicalKind::Join, std::move(lj));
        }

        // 2) WHERE → Filter
        if (select.where.has_value()) {
            cur = LogicalPlan::make_filter(*select.where, std::move(cur));
        }

        // 3) GROUP BY + 聚合 → Aggregate
        bool has_agg = false;
        for (const auto& it: select.projections) {
            if (it.is_aggregate()) {
                has_agg = true;
                break;
            }
        }
        if (!select.group_by.empty() || has_agg || select.having.has_value()) {
            LogicalAggregate la;
            la.group_by.reserve(select.group_by.size());
            for (const auto& c: select.group_by)
                la.group_by.emplace_back(c);
            int agg_idx = 0;
            for (const auto& it: select.projections) {
                if (!it.is_aggregate())
                    continue;
                std::string out = it.alias.value_or("agg_" + std::to_string(agg_idx));
                la.aggregates.push_back(to_agg_item(std::get<AggregateExpr>(it.value), std::move(out)));
                ++agg_idx;
            }
            // 从 HAVING 提取聚合表达式，确保它们被计算。
            if (select.having.has_value()) {
                std::vector<AggregateExpr> having_aggs;
                auto collect = [](auto& self, const BoolExpr& b, std::vector<AggregateExpr>& out) -> void {
                    if (b.kind == BoolExpr::Kind::Comparison && b.cmp.has_value()) {
                        if (auto* a = std::get_if<AggregateExpr>(&b.cmp->lhs)) out.push_back(*a);
                        if (auto* a = std::get_if<AggregateExpr>(&b.cmp->rhs)) out.push_back(*a);
                    } else if (b.left) {
                        self(self, *b.left, out);
                        if (b.right) self(self, *b.right, out);
                    }
                };
                collect(collect, *select.having, having_aggs);
                for (auto& ha : having_aggs) {
                    bool dup = false;
                    for (const auto& a : la.aggregates)
                        if (a.func == ha.func) { dup = true; break; }
                    if (!dup) {
                        la.aggregates.push_back(to_agg_item(ha, "agg_" + std::to_string(agg_idx)));
                        ++agg_idx;
                    }
                }
            }
            la.having = select.having;
            la.child = std::move(cur);
            cur = LogicalPlan::make(LogicalKind::Aggregate, std::move(la));
        }

        // 4) ORDER BY → Sort
        if (!select.order_by.empty()) {
            LogicalSort ls;
            ls.keys.reserve(select.order_by.size());
            for (const auto& ob: select.order_by) {
                LogicalSortKey k;
                if (auto* e = std::get_if<Expression>(&ob.key))
                    k.expr = *e;
                else
                    k.expr = std::get<AggregateExpr>(ob.key);
                k.ascending = ob.asc;
                ls.keys.push_back(std::move(k));
            }
            ls.child = std::move(cur);
            cur = LogicalPlan::make(LogicalKind::Sort, std::move(ls));
        }

        // 5) LIMIT/OFFSET → Limit
        if (select.limit.has_value() || select.offset.has_value()) {
            std::optional<std::size_t> off;
            std::optional<std::size_t> lim;
            if (select.offset.has_value() && *select.offset >= 0)
                off = static_cast<std::size_t>(*select.offset);
            if (select.limit.has_value() && *select.limit >= 0)
                lim = static_cast<std::size_t>(*select.limit);
            cur = LogicalPlan::make_limit(off, lim, std::move(cur));
        }

        // 6) SELECT 投影 → Project（最外层）。SELECT *：展开为所有源表列。
        std::vector<LogicalColumn> cols;
        cols.reserve(select.projections.size());
        int idx = 0;
        // 收集所有源表（base + joins），用于 * 展开（保持 FROM 顺序）。
        std::vector<std::shared_ptr<Table>> source_tables;
        std::vector<std::string> source_aliases;
        source_tables.push_back(base_table);
        source_aliases.push_back(base_alias);
        for (const auto& j: select.joins) {
            auto rt = resolve_table(j.table);
            source_tables.push_back(rt);
            source_aliases.push_back(j.alias.value_or(std::string{}));
        }
        auto is_star = [](const SelectStmt::SelectItem& it) -> bool {
            if (auto* e = std::get_if<Expression>(&it.value)) {
                if (auto* c = std::get_if<ColumnRef>(e))
                    return c->name == "*";
            }
            return false;
        };
        for (const auto& it: select.projections) {
            if (is_star(it)) {
                // 展开为所有源表的所有列
                for (std::size_t ti = 0; ti < source_tables.size(); ++ti) {
                    const auto& tbl = source_tables[ti];
                    const auto& alias = source_aliases[ti];
                    for (const auto& c: tbl->columns()) {
                        LogicalColumn lc;
                        ColumnRef ref;
                        ref.table = alias.empty() ? tbl->name() : alias;
                        ref.name = c.name;
                        lc.expr = ref;
                        lc.output_name = c.name;
                        cols.push_back(std::move(lc));
                    }
                }
                continue;
            }
            LogicalColumn lc;
            lc.expr = item_to_expr(it);
            if (it.alias.has_value()) {
                lc.output_name = *it.alias;
            } else if (auto* col = std::get_if<ColumnRef>(&lc.expr)) {
                lc.output_name = col->name;
            } else if (auto* agg = std::get_if<AggregateExpr>(&lc.expr)) {
                lc.output_name = agg->to_string();
            } else {
                lc.output_name = "col_" + std::to_string(idx);
            }
            cols.push_back(std::move(lc));
            ++idx;
        }
        cur = LogicalPlan::make_project(std::move(cols), std::move(cur));
        if (select.distinct)
            std::get<LogicalProject>(cur->node).distinct = true;
        return cur;
    }

    /**
     * @brief 将 INSERT 语句翻译为 LogicalDML（Kind::Insert）+ LogicalValues 逻辑计划。
     */
    LogicalPlanPtr LogicalPlanner::plan_insert(const InsertStmt& ins) {
        auto tbl = resolve_table(ins.table);
        LogicalDML dml;
        dml.kind = LogicalDML::Kind::Insert;
        dml.table = tbl;
        dml.columns = ins.columns;
        dml.source = LogicalPlan::make(LogicalKind::Values, LogicalValues{ ins.rows });
        return LogicalPlan::make(LogicalKind::DML, std::move(dml));
    }

    /**
     * @brief 将 UPDATE 语句翻译为 LogicalDML（Kind::Update）逻辑计划（含 WHERE 过滤子树）。
     */
    LogicalPlanPtr LogicalPlanner::plan_update(const UpdateStmt& upd) {
        auto tbl = resolve_table(upd.table);
        LogicalDML dml;
        dml.kind = LogicalDML::Kind::Update;
        dml.table = tbl;
        for (const auto& a: upd.assignments) {
            dml.columns.push_back(a.column);
            dml.set_exprs.push_back(a.value);
        }
        dml.where = upd.where;
        // source 为表扫描，便于规则统一处理（与 PG 的 ModifyTable + childPlan 思路一致）
        dml.source = LogicalPlan::make_scan(tbl);
        if (upd.where.has_value()) {
            dml.source = LogicalPlan::make_filter(*upd.where, std::move(dml.source));
        }
        return LogicalPlan::make(LogicalKind::DML, std::move(dml));
    }

    /**
     * @brief 将 DELETE 语句翻译为 LogicalDML（Kind::Delete）逻辑计划（含 WHERE 过滤子树）。
     */
    LogicalPlanPtr LogicalPlanner::plan_delete(const DeleteStmt& del) {
        auto tbl = resolve_table(del.table);
        LogicalDML dml;
        dml.kind = LogicalDML::Kind::Delete;
        dml.table = tbl;
        dml.where = del.where;
        dml.source = LogicalPlan::make_scan(tbl);
        if (del.where.has_value()) {
            dml.source = LogicalPlan::make_filter(*del.where, std::move(dml.source));
        }
        return LogicalPlan::make(LogicalKind::DML, std::move(dml));
    }

} // namespace corodb::opt
