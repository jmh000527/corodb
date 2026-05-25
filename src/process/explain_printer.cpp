// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file explain_printer.cpp
// @brief EXPLAIN output formatter — PostgreSQL-style plain-text plan trees.

#include "corodb/process/explain_printer.h"

#include <iomanip>
#include <sstream>

namespace corodb {

    namespace {

        std::string value_to_string(const Value& v) {
            return std::visit(
                    [](const auto& val) -> std::string {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, NullValue>) {
                            return "NULL";
                        } else if constexpr (std::is_same_v<T, int64_t>) {
                            return std::to_string(val);
                        } else if constexpr (std::is_same_v<T, double>) {
                            return std::to_string(val);
                        } else if constexpr (std::is_same_v<T, std::string>) {
                            return "'" + val + "'";
                        }
                        return "?";
                    },
                    v);
        }

        std::string join_type_string(JoinType jt) {
            switch (jt) {
                case JoinType::Inner: return "inner";
                case JoinType::Left:  return "left";
                case JoinType::Right: return "right";
                case JoinType::Full:  return "full";
                default:              return "inner";
            }
        }

        std::string to_string(const ColumnRef& col) {
            return col.table.empty() ? col.name : col.table + "." + col.name;
        }

        std::string to_string(const AggregateExpr& agg) {
            const char* name = "unknown";
            switch (agg.func) {
                case AggFunc::Count: name = "count"; break;
                case AggFunc::Sum:   name = "sum";   break;
                case AggFunc::Avg:   name = "avg";   break;
                case AggFunc::Min:   name = "min";   break;
                case AggFunc::Max:   name = "max";   break;
            }
            return agg.arg.has_value() ? std::string(name) + "(" + to_string(*agg.arg) + ")"
                                       : std::string(name) + "(*)";
        }

        std::string to_string(const Expression& expr);

        std::string to_string(const std::shared_ptr<BinaryExpr>& bin) {
            if (!bin) return "<invalid>";
            auto op = [](BinaryExpr::Op o) {
                switch (o) { case BinaryExpr::Op::Add: return "+"; case BinaryExpr::Op::Sub: return "-";
                             case BinaryExpr::Op::Mul: return "*"; case BinaryExpr::Op::Div: return "/";
                             default: return "?"; }
            };
            return "(" + to_string(bin->lhs) + " " + op(bin->op) + " " + to_string(bin->rhs) + ")";
        }

        std::string to_string(const Expression& expr) {
            return std::visit(
                    [&](const auto& node) {
                        using T = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<T, ColumnRef>) return to_string(node);
                        if constexpr (std::is_same_v<T, Literal>) {
                            if (std::holds_alternative<int64_t>(node.value))
                                return std::to_string(std::get<int64_t>(node.value));
                            return "'" + std::get<std::string>(node.value) + "'";
                        }
                        if constexpr (std::is_same_v<T, std::shared_ptr<BinaryExpr>>) return to_string(node);
                        if constexpr (std::is_same_v<T, AggregateExpr>) return to_string(node);
                        return std::string("<unknown>");
                    },
                    expr);
        }

        std::string to_string(const BoolExpr& expr) {
            switch (expr.kind) {
                case BoolExpr::Kind::Comparison: {
                    if (!expr.cmp.has_value()) return "<cmp?>";
                    const auto& c = *expr.cmp;
                    auto op = [](CompareOp o) {
                        switch (o) { case CompareOp::Eq: return "="; case CompareOp::Ne: return "!=";
                                     case CompareOp::Lt: return "<"; case CompareOp::Le: return "<=";
                                     case CompareOp::Gt: return ">"; case CompareOp::Ge: return ">=";
                                     default: return "?"; }
                    };
                    return to_string(c.lhs) + " " + op(c.op) + " " + to_string(c.rhs);
                }
                case BoolExpr::Kind::And:
                    return "(" + to_string(*expr.left) + " AND " + to_string(*expr.right) + ")";
                case BoolExpr::Kind::Or:
                    return "(" + to_string(*expr.left) + " OR " + to_string(*expr.right) + ")";
                case BoolExpr::Kind::Not:
                    return "(NOT " + to_string(*expr.left) + ")";
                default: return "<bool?>";
            }
        }

        std::string describe_project_item(const SelectStmt::SelectItem& item) {
            std::string rhs;
            if (std::holds_alternative<AggregateExpr>(item.value))
                rhs = to_string(std::get<AggregateExpr>(item.value));
            else
                rhs = to_string(std::get<Expression>(item.value));
            return item.alias.has_value() ? rhs + " AS " + *item.alias : rhs;
        }

        std::string join_vec(const std::vector<std::string>& parts) {
            std::string res;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i) res += ", ";
                res += parts[i];
            }
            return res;
        }

        // ── PostgreSQL-style indentation ─────────────────────────────────
        static std::string indent(int depth) {
            if (depth == 0) return "";
            return std::string(static_cast<std::size_t>(depth * 2), ' ') + "->  ";
        }
        static std::string attr_indent(int depth) {
            return std::string(static_cast<std::size_t>(depth * 2 + 4), ' ');
        }

        void emit_plan(const PlanNode* plan, std::ostringstream& oss, int depth) {
            const std::string in = indent(depth);
            const std::string ai = attr_indent(depth);

            if (const auto* seq = dynamic_cast<const SeqScanPlan*>(plan)) {
                oss << in << "Seq Scan on " << seq->table->name() << "\n";
                return;
            }
            if (const auto* idx = dynamic_cast<const IndexScanPlan*>(plan)) {
                oss << in << "Index Scan on " << idx->table->name() << "\n";
                oss << ai << "Index Cond: (" << idx->column << " = " << value_to_string(idx->key) << ")\n";
                return;
            }
            if (const auto* fil = dynamic_cast<const FilterPlan*>(plan)) {
                oss << in << "Filter: (" << to_string(fil->predicate) << ")\n";
                emit_plan(fil->child.get(), oss, depth + 1);
                return;
            }
            if (const auto* proj = dynamic_cast<const ProjectPlan*>(plan)) {
                std::vector<std::string> cols;
                for (const auto& c: proj->columns) cols.push_back(describe_project_item(c));
                oss << in << "Project [" << join_vec(cols) << "]\n";
                emit_plan(proj->child.get(), oss, depth + 1);
                return;
            }
            if (const auto* hash = dynamic_cast<const HashJoinPlan*>(plan)) {
                oss << in << "Hash Join (" << join_type_string(hash->type) << ")\n";
                oss << ai << "Hash Cond: (" << to_string(hash->left_key) << " = "
                    << to_string(hash->right_key) << ")\n";
                if (hash->residual.has_value())
                    oss << ai << "Join Filter: (" << to_string(*hash->residual) << ")\n";
                emit_plan(hash->left.get(), oss, depth + 1);
                emit_plan(hash->right.get(), oss, depth + 1);
                return;
            }
            if (const auto* merge = dynamic_cast<const MergeJoinPlan*>(plan)) {
                oss << in << "Merge Join (" << join_type_string(merge->type) << ")\n";
                oss << ai << "Merge Cond: (" << to_string(merge->left_key) << " = "
                    << to_string(merge->right_key) << ")\n";
                if (merge->residual.has_value())
                    oss << ai << "Join Filter: (" << to_string(*merge->residual) << ")\n";
                emit_plan(merge->left.get(), oss, depth + 1);
                emit_plan(merge->right.get(), oss, depth + 1);
                return;
            }
            if (const auto* loop = dynamic_cast<const NestedLoopJoinPlan*>(plan)) {
                oss << in << "Nested Loop (" << join_type_string(loop->type) << ")\n";
                oss << ai << "Join Filter: (" << to_string(loop->on) << ")\n";
                emit_plan(loop->left.get(), oss, depth + 1);
                emit_plan(loop->right.get(), oss, depth + 1);
                return;
            }
            if (const auto* agg = dynamic_cast<const AggregatePlan*>(plan)) {
                oss << in << (agg->strategy == AggregatePlan::Strategy::Sort ? "Aggregate (sort)"
                                                                             : "Hash Aggregate") << "\n";
                if (!agg->group_by.empty()) {
                    std::vector<std::string> groups;
                    for (const auto& g: agg->group_by) groups.push_back(to_string(g));
                    oss << ai << "Group Key: " << join_vec(groups) << "\n";
                }
                if (agg->having.has_value())
                    oss << ai << "Having: (" << to_string(*agg->having) << ")\n";
                emit_plan(agg->child.get(), oss, depth + 1);
                return;
            }
            if (const auto* ord = dynamic_cast<const OrderByPlan*>(plan)) {
                std::vector<std::string> items;
                for (const auto& it: ord->items) {
                    std::string key = std::holds_alternative<Expression>(it.key)
                                      ? to_string(std::get<Expression>(it.key))
                                      : to_string(std::get<AggregateExpr>(it.key));
                    items.push_back(key + (it.asc ? " ASC" : " DESC"));
                }
                oss << in << "Sort\n";
                oss << ai << "Sort Key: " << join_vec(items) << "\n";
                emit_plan(ord->child.get(), oss, depth + 1);
                return;
            }
            if (const auto* lim = dynamic_cast<const LimitPlan*>(plan)) {
                oss << in << "Limit";
                if (lim->limit) oss << " (rows=" << *lim->limit << ")";
                if (lim->offset && *lim->offset > 0) oss << " (offset=" << *lim->offset << ")";
                oss << "\n";
                emit_plan(lim->child.get(), oss, depth + 1);
                return;
            }
            if (const auto* ins = dynamic_cast<const InsertPlan*>(plan)) {
                oss << in << "Insert on " << ins->table->name() << "\n";
                return;
            }
            if (const auto* upd = dynamic_cast<const UpdatePlan*>(plan)) {
                oss << in << "Update on " << upd->table->name() << "\n";
                if (upd->where.has_value())
                    oss << ai << "Filter: (" << to_string(*upd->where) << ")\n";
                return;
            }
            if (const auto* del = dynamic_cast<const DeletePlan*>(plan)) {
                oss << in << "Delete on " << del->table->name() << "\n";
                if (del->where.has_value())
                    oss << ai << "Filter: (" << to_string(*del->where) << ")\n";
                return;
            }
            oss << in << "<unknown>\n";
        }

    } // namespace

    /**
     * @brief 将物理计划递归格式化为 PostgreSQL 风格的纯文本计划树。
     * @param plan 物理计划根节点。
     * @return 带树形缩进和 "->  " 箭头的多行文本。
     */
    std::string ExplainPrinter::format(const PlanNode* plan) {
        std::ostringstream oss;
        emit_plan(plan, oss, 0);
        return oss.str();
    }

    /**
     * @brief 生成 EXPLAIN 输出的行流（仅物理计划文本，无逻辑计划）。
     *
     * 每行是一个包含 "QUERY PLAN" 绑定的 Record，由客户端渲染为 ASCII 表格。
     * @param plan 物理计划根节点。
     */
    std::generator<Record> ExplainPrinter::render(const PlanNode* plan) {
        std::string text = ExplainPrinter::format(plan);
        std::istringstream iss(text);
        std::string line;
        std::vector<Binding> bindings;
        bindings.push_back(Binding{ "", "QUERY PLAN", 0 });

        auto make = [&](std::string val) -> Record {
            Record r;
            r.bindings = bindings;
            r.values.push_back(std::move(val));
            return r;
        };

        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            co_yield make(std::move(line));
        }
    }

    /**
     * @brief EXPLAIN 双栏模式的兼容入口（当前退化为仅输出物理计划）。
     *
     * 历史版本同时输出逻辑和物理计划；现在统一为仅物理计划。
     * @param physical 物理计划根节点。
     */
    std::generator<Record> ExplainPrinter::render_dual(std::string, const PlanNode* physical) {
        return render(physical);
    }

    /**
     * @brief EXPLAIN ANALYZE 输出：物理计划树 + 各算子运行时统计 + 总耗时。
     *
     * 实际执行查询并收集 QueryStats，然后与计划树一起渲染。
     * @param physical 物理计划根节点。
     * @param stats    查询执行收集的运行时统计。
     */
    std::generator<Record> ExplainPrinter::render_analyze(std::string, const PlanNode* physical,
                                                          const QueryStats& stats) {
        std::vector<Binding> bindings;
        bindings.push_back(Binding{ "", "QUERY PLAN", 0 });
        auto emit = [&](std::string s) -> Record {
            Record r;
            r.bindings = bindings;
            r.values.push_back(std::move(s));
            return r;
        };

        {
            std::string text = ExplainPrinter::format(physical);
            std::istringstream iss(text);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                co_yield emit(std::move(line));
            }
        }
        co_yield emit("");
        co_yield emit("Execution Time: " + std::to_string(stats.total_ms) + " ms");
        for (const auto& op : stats.operators) {
            if (op.rows > 0 || op.elapsed_ms > 0.0) {
                co_yield emit("  " + op.name + ": rows=" + std::to_string(op.rows) +
                              "  time=" + std::to_string(op.elapsed_ms) + " ms");
            }
        }
    }

} // namespace corodb
