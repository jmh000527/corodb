// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file logical_plan.cpp
// @brief 逻辑查询计划节点的克隆与调试输出实现。

#include "corodb/plan/logical_plan.h"

#include <sstream>

namespace corodb::opt {

    namespace {
        struct CloneVisitor {
            LogicalPlanPtr operator()(const LogicalScan& s) const {
                return LogicalPlan::make(LogicalKind::Scan, LogicalScan{ s.table, s.alias });
            }
            LogicalPlanPtr operator()(const LogicalFilter& f) const {
                return LogicalPlan::make(LogicalKind::Filter, LogicalFilter{ f.predicate, clone(*f.child) });
            }
            LogicalPlanPtr operator()(const LogicalProject& p) const {
                return LogicalPlan::make(LogicalKind::Project, LogicalProject{ p.columns, clone(*p.child) });
            }
            LogicalPlanPtr operator()(const LogicalJoin& j) const {
                return LogicalPlan::make(LogicalKind::Join,
                                         LogicalJoin{ j.join_type, j.on, clone(*j.left), clone(*j.right) });
            }
            LogicalPlanPtr operator()(const LogicalAggregate& a) const {
                return LogicalPlan::make(LogicalKind::Aggregate,
                                         LogicalAggregate{ a.group_by, a.aggregates, a.having, clone(*a.child) });
            }
            LogicalPlanPtr operator()(const LogicalSort& s) const {
                return LogicalPlan::make(LogicalKind::Sort, LogicalSort{ s.keys, clone(*s.child) });
            }
            LogicalPlanPtr operator()(const LogicalLimit& l) const {
                return LogicalPlan::make(LogicalKind::Limit, LogicalLimit{ l.offset, l.limit, clone(*l.child) });
            }
            LogicalPlanPtr operator()(const LogicalValues& v) const {
                return LogicalPlan::make(LogicalKind::Values, LogicalValues{ v.rows });
            }
            LogicalPlanPtr operator()(const LogicalDML& d) const {
                LogicalDML c;
                c.kind = d.kind;
                c.table = d.table;
                c.source = d.source ? clone(*d.source) : nullptr;
                c.columns = d.columns;
                c.set_exprs = d.set_exprs;
                c.where = d.where;
                return LogicalPlan::make(LogicalKind::DML, std::move(c));
            }
            LogicalPlanPtr operator()(const LogicalDDL& d) const {
                return LogicalPlan::make(LogicalKind::DDL, LogicalDDL{ d.original });
            }
        };

        // ── Expression / BoolExpr formatting ─────────────────────────────────

        std::string fmt_expr(const Expression& e);

        std::string fmt_agg_func(AggFunc f) {
            switch (f) {
                case AggFunc::Count:
                    return "count";
                case AggFunc::Sum:
                    return "sum";
                case AggFunc::Avg:
                    return "avg";
                case AggFunc::Min:
                    return "min";
                case AggFunc::Max:
                    return "max";
                default:
                    return "?";
            }
        }

        std::string fmt_expr(const Expression& e) {
            return std::visit(
                    [](const auto& v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, ColumnRef>) {
                            return v.table.empty() ? v.name : v.table + "." + v.name;
                        } else if constexpr (std::is_same_v<T, Literal>) {
                            if (std::holds_alternative<int64_t>(v.value))
                                return std::to_string(std::get<int64_t>(v.value));
                            return "'" + std::get<std::string>(v.value) + "'";
                        } else if constexpr (std::is_same_v<T, std::shared_ptr<BinaryExpr>>) {
                            if (!v)
                                return "<nil>";
                            const char* op = [&]() -> const char* {
                                switch (v->op) {
                                    case BinaryExpr::Op::Add:
                                        return "+";
                                    case BinaryExpr::Op::Sub:
                                        return "-";
                                    case BinaryExpr::Op::Mul:
                                        return "*";
                                    case BinaryExpr::Op::Div:
                                        return "/";
                                    default:
                                        return "?";
                                }
                            }();
                            return "(" + fmt_expr(v->lhs) + " " + op + " " + fmt_expr(v->rhs) + ")";
                        } else if constexpr (std::is_same_v<T, AggregateExpr>) {
                            std::string name = fmt_agg_func(v.func);
                            return v.arg.has_value() ? name + "(" + fmt_expr(*v.arg) + ")" : name + "(*)";
                        } else {
                            return "<expr?>";
                        }
                    },
                    e);
        }

        std::string fmt_bool(const BoolExpr& b) {
            switch (b.kind) {
                case BoolExpr::Kind::Comparison: {
                    if (!b.cmp)
                        return "<cmp?>";
                    const char* op = [&]() -> const char* {
                        switch (b.cmp->op) {
                            case CompareOp::Eq:
                                return "=";
                            case CompareOp::Ne:
                                return "!=";
                            case CompareOp::Lt:
                                return "<";
                            case CompareOp::Le:
                                return "<=";
                            case CompareOp::Gt:
                                return ">";
                            case CompareOp::Ge:
                                return ">=";
                            default:
                                return "?";
                        }
                    }();
                    return fmt_expr(b.cmp->lhs) + " " + op + " " + fmt_expr(b.cmp->rhs);
                }
                case BoolExpr::Kind::And:
                    return "(" + fmt_bool(*b.left) + " AND " + fmt_bool(*b.right) + ")";
                case BoolExpr::Kind::Or:
                    return "(" + fmt_bool(*b.left) + " OR " + fmt_bool(*b.right) + ")";
                case BoolExpr::Kind::Not:
                    return "(NOT " + fmt_bool(*b.left) + ")";
                default:
                    return "<bool?>";
            }
        }

        /** @brief 将字符串列表用逗号连接。 */
        static std::string join_strs(const std::vector<std::string>& v) {
            std::string res;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i)
                    res += ", ";
                res += v[i];
            }
            return res;
        }

        // ── PostgreSQL-style indentation (mirrors explain.cpp) ────────────────
        /** @brief 逻辑计划树节点前缀（PostgreSQL 风格缩进）。 */
        std::string lp_node_prefix(int d) {
            if (d == 0) return " ";
            return std::string(static_cast<std::size_t>(3 + 6 * (d - 1)), ' ') + "->  ";
        }
        /** @brief 逻辑计划树属性前缀（对齐到节点下方）。 */
        std::string lp_attr_prefix(int d) {
            int n = (d == 0) ? 3 : 3 + 6 * d;
            return std::string(static_cast<std::size_t>(n), ' ');
        }

        // ── PrintVisitor ──────────────────────────────────────────────────────

        struct PrintVisitor {
            std::ostringstream& oss;
            int depth;

            std::string np() const {
                return lp_node_prefix(depth);
            }
            std::string ap() const {
                return lp_attr_prefix(depth);
            }

            void print_child(const LogicalPlan& plan) const {
                std::visit(PrintVisitor{ oss, depth + 1 }, plan.node);
            }

            void operator()(const LogicalScan& s) const {
                oss << np() << "Scan on " << (s.table ? s.table->name() : "?");
                if (!s.alias.empty())
                    oss << " " << s.alias;
                oss << "\n";
            }

            void operator()(const LogicalFilter& f) const {
                oss << np() << "Filter\n";
                oss << ap() << "Filter: (" << fmt_bool(f.predicate) << ")\n";
                print_child(*f.child);
            }

            void operator()(const LogicalProject& p) const {
                std::vector<std::string> cols;
                for (const auto& c: p.columns) {
                    std::string s = fmt_expr(c.expr);
                    if (!c.output_name.empty() && c.output_name != s)
                        s += " AS " + c.output_name;
                    cols.push_back(std::move(s));
                }
                oss << np() << "Project  [" << join_strs(cols) << "]\n";
                print_child(*p.child);
            }

            void operator()(const LogicalJoin& j) const {
                oss << np() << "Join  (" << join_type_name(j.join_type) << ")";
                if (j.on.has_value())
                    oss << "\n" << ap() << "Join Cond: (" << fmt_bool(*j.on) << ")";
                oss << "\n";
                print_child(*j.left);
                print_child(*j.right);
            }

            void operator()(const LogicalAggregate& a) const {
                oss << np() << "Aggregate\n";
                if (!a.group_by.empty()) {
                    std::vector<std::string> groups;
                    for (const auto& g: a.group_by)
                        groups.push_back(fmt_expr(g));
                    oss << ap() << "Group Key: " << join_strs(groups) << "\n";
                }
                if (!a.aggregates.empty()) {
                    std::vector<std::string> aggs;
                    for (const auto& ai: a.aggregates) {
                        std::string s = fmt_agg_func(ai.func);
                        s += ai.arg.has_value() ? "(" + fmt_expr(*ai.arg) + ")" : "(*)";
                        aggs.push_back(std::move(s));
                    }
                    oss << ap() << "Agg: " << join_strs(aggs) << "\n";
                }
                if (a.having.has_value())
                    oss << ap() << "Having: (" << fmt_bool(*a.having) << ")\n";
                print_child(*a.child);
            }

            void operator()(const LogicalSort& s) const {
                std::vector<std::string> keys;
                for (const auto& k: s.keys)
                    keys.push_back(fmt_expr(k.expr) + (k.ascending ? " ASC" : " DESC"));
                oss << np() << "Sort\n";
                oss << ap() << "Sort Key: " << join_strs(keys) << "\n";
                print_child(*s.child);
            }

            void operator()(const LogicalLimit& l) const {
                oss << np() << "Limit";
                if (l.limit)
                    oss << "  rows=" << *l.limit;
                if (l.offset && *l.offset > 0)
                    oss << "  offset=" << *l.offset;
                oss << "\n";
                print_child(*l.child);
            }

            void operator()(const LogicalValues& v) const {
                oss << np() << "Values  (" << v.rows.size() << " rows)\n";
            }

            void operator()(const LogicalDML& d) const {
                const char* k = "?";
                switch (d.kind) {
                    case LogicalDML::Kind::Insert:
                        k = "Insert";
                        break;
                    case LogicalDML::Kind::Update:
                        k = "Update";
                        break;
                    case LogicalDML::Kind::Delete:
                        k = "Delete";
                        break;
                }
                oss << np() << k << " on " << (d.table ? d.table->name() : "?") << "\n";
                if (d.where.has_value())
                    oss << ap() << "Filter: (" << fmt_bool(*d.where) << ")\n";
                if (d.source)
                    print_child(*d.source);
            }

            void operator()(const LogicalDDL&) const {
                oss << np() << "DDL\n";
            }
        };
    } // namespace

    /**
     * @brief 深度克隆逻辑计划树（递归复制所有节点）。
     * @param plan 待克隆的逻辑计划。
     * @return 完整克隆后的新计划树。
     */
    LogicalPlanPtr clone(const LogicalPlan& plan) {
        return std::visit(CloneVisitor{}, plan.node);
    }

    /**
     * @brief 将逻辑计划树格式化为 PostgreSQL 风格的文本（EXPLAIN 输出）。
     * @param plan 逻辑计划根节点。
     * @param indent 起始缩进深度（通常为 0）。
     * @return 可读性说明字符串。
     */
    std::string to_string(const LogicalPlan& plan, int indent) {
        std::ostringstream oss;
        std::visit(PrintVisitor{ oss, indent }, plan.node);
        return oss.str();
    }

} // namespace corodb::opt
