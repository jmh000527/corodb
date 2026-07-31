// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file physical_planner.cpp
// @brief 物理查询计划生成器的实现。

#include "corodb/optimizer/physical/physical_planner.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/table.h"

namespace corodb::opt {

    PhysicalPlanner::PhysicalPlanner() = default;

    PhysicalPlanner::PhysicalPlanner(Catalog& catalog, StorageEngine* storage)
        : catalog_{ &catalog }, storage_{ storage } {
    }

    PhysicalPlanner::~PhysicalPlanner() = default;

    namespace {
        /** @brief 从 Literal 表达式中提取常量 Value（INSERT/Values 场景）。 */
        Value materialize_constant(const Expression& expr) {
            if (auto* lit = std::get_if<Literal>(&expr))
                return lit->value;
            throw std::runtime_error("[PhysicalPlanner] Only literal values supported in INSERT/Values");
        }

        /** @brief 反转比较操作符方向（将 lit OP col 归一化为 col OP' lit）。 */
        CompareOp reverse_compare_op(CompareOp op) {
            switch (op) {
                case CompareOp::Lt:
                    return CompareOp::Gt;
                case CompareOp::Le:
                    return CompareOp::Ge;
                case CompareOp::Gt:
                    return CompareOp::Lt;
                case CompareOp::Ge:
                    return CompareOp::Le;
                default:
                    return op; // Eq/Ne 等对称或不处理
            }
        }

        /** @brief 按列名查找列索引，未找到抛异常。 */
        std::size_t resolve_column_index(const Table& table, const std::string& col) {
            const auto& cols = table.columns();
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (cols[i].name == col)
                    return i;
            }
            throw std::runtime_error("[PhysicalPlanner] Unknown column: " + col + " in " + table.name());
        }

        /** @brief 递归收集计划子树中所有表名和别名（用于 Join 等值键归属判断）。 */
        void collect_plan_tables(const LogicalPlan& p, std::unordered_set<std::string>& out) {
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

        /** @brief LogicalColumn 列表 → SelectItem 列表（Project/Aggregate 共用）。 */
        std::vector<SelectStmt::SelectItem> cols_to_items(const std::vector<LogicalColumn>& cs) {
            std::vector<SelectStmt::SelectItem> out;
            out.reserve(cs.size());
            for (const auto& c: cs) {
                SelectStmt::SelectItem it;
                if (auto* a = std::get_if<AggregateExpr>(&c.expr)) {
                    it.value = *a;
                } else {
                    it.value = c.expr;
                }
                if (!c.output_name.empty())
                    it.alias = c.output_name;
                out.push_back(std::move(it));
            }
            return out;
        }

        /** @brief 检查 ON 是否为简单等值连接 (col = col)，按表归属分配左右键。 */
        bool try_extract_equi_keys(const BoolExpr& on, const std::unordered_set<std::string>& left_tabs,
                                   const std::unordered_set<std::string>& right_tabs, ColumnRef& lkey,
                                   ColumnRef& rkey) {
            if (on.kind != BoolExpr::Kind::Comparison || !on.cmp.has_value())
                return false;
            const auto& cmp = *on.cmp;
            if (cmp.op != CompareOp::Eq)
                return false;
            auto* a = std::get_if<ColumnRef>(&cmp.lhs);
            auto* b = std::get_if<ColumnRef>(&cmp.rhs);
            if (!a || !b)
                return false;
            if (a->table.empty() || b->table.empty())
                return false;
            // a 在 left, b 在 right
            if (left_tabs.count(a->table) && right_tabs.count(b->table)) {
                lkey = *a;
                rkey = *b;
                return true;
            }
            // 反之
            if (right_tabs.count(a->table) && left_tabs.count(b->table)) {
                lkey = *b;
                rkey = *a;
                return true;
            }
            return false;
        }
    } // namespace

    /**
     * @brief 将逻辑计划树翻译为可执行的物理计划树。
     * @param lp 逻辑计划根节点。
     * @return 物理计划根节点。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::plan(const LogicalPlan& lp) {
        return visit(lp);
    }

    /**
     * @brief 按节点类型分发到对应的 build_* 方法。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::visit(const LogicalPlan& lp) {
        return std::visit(
                [&](const auto& n) -> std::unique_ptr<PlanNode> {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LogicalScan>)
                        return build_scan(n);
                    else if constexpr (std::is_same_v<T, LogicalFilter>)
                        return build_filter(n);
                    else if constexpr (std::is_same_v<T, LogicalProject>)
                        return build_project(n);
                    else if constexpr (std::is_same_v<T, LogicalJoin>)
                        return build_join(n);
                    else if constexpr (std::is_same_v<T, LogicalAggregate>)
                        return build_aggregate(n);
                    else if constexpr (std::is_same_v<T, LogicalSort>)
                        return build_sort(n);
                    else if constexpr (std::is_same_v<T, LogicalLimit>)
                        return build_limit(n);
                    else if constexpr (std::is_same_v<T, LogicalDML>)
                        return build_dml(n);
                    else if constexpr (std::is_same_v<T, LogicalValues>) {
                        throw std::runtime_error("[PhysicalPlanner] LogicalValues outside DML context not supported");
                    } else if constexpr (std::is_same_v<T, LogicalDDL>) {
                        return build_ddl(n);
                    }
                },
                lp.node);
    }

    /**
     * @brief 将 LogicalScan 翻译为 SeqScanPlan。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_scan(const LogicalScan& s) {
        return std::make_unique<SeqScanPlan>(s.table, s.alias);
    }

    /**
     * @brief 代价决策（OPT-2/3，直方图升级）：范围谓词是否值得走索引。
     *
     * 小表恒走索引；大表优先用有序索引的精确分布探针（index_range_fraction，倾斜数据下
     * 也准确；计数超阈即短路）；探针不可用时回退 min/max 线性插值。覆盖率 >50% 视为
     * 非选择性——超集索引逐 pk 点查 + 可见性重查比一次顺序流式扫描更贵。
     */
    static bool range_worth_index(const Table& t, const std::string& col, const std::optional<Value>& low,
                                  bool low_inc, const std::optional<Value>& high, bool high_inc) {
        constexpr std::size_t kSmallTable = 128;
        constexpr double kThreshold = 0.5;
        if (t.estimated_row_count() < kSmallTable)
            return true;
        // 精确分布探针（任意可比较类型，含字符串；倾斜分布下准确）。
        if (auto frac = t.index_range_fraction(col, low, low_inc, high, high_inc, kThreshold))
            return *frac <= kThreshold;
        // 回退：min/max 线性插值（仅数值）。
        auto mm = t.index_min_max(col);
        if (!mm)
            return true;
        auto to_num = [](const Value& v) -> std::optional<double> {
            if (const auto* i = std::get_if<int64_t>(&v))
                return static_cast<double>(*i);
            if (const auto* d = std::get_if<double>(&v))
                return *d;
            return std::nullopt;
        };
        auto minv = to_num(mm->first);
        auto maxv = to_num(mm->second);
        if (!minv || !maxv || *maxv <= *minv)
            return true;
        double lo = *minv;
        double hi = *maxv;
        if (low.has_value()) {
            if (auto l = to_num(*low))
                lo = std::max(lo, *l);
        }
        if (high.has_value()) {
            if (auto h = to_num(*high))
                hi = std::min(hi, *h);
        }
        if (hi <= lo)
            return true; // 空/极窄范围：索引更优
        const double frac = (hi - lo) / (*maxv - *minv);
        return frac <= kThreshold;
    }

    /**
     * @brief 代价决策（OPT-6）：等值谓词是否值得走索引。
     *
     * 低基数列（NDV 极小，如布尔/枚举）上等值命中约 rows/NDV 行；NDV<4 时单键预期覆盖
     * ≥25% 行，超集索引逐 pk 点查比顺序扫描更贵 → 落回 SeqScan。小表/高基数维持走索引。
     * NDV 计数带上限（只需判断是否 <4，O(4·log n)）。
     */
    static bool equality_worth_index(const Table& t, const std::string& col) {
        constexpr std::size_t kSmallTable = 128;
        constexpr std::size_t kLowNdv = 4;
        if (t.estimated_row_count() < kSmallTable)
            return true;
        const std::size_t ndv = t.index_distinct_count(col, kLowNdv);
        return ndv == 0 || ndv >= kLowNdv; // ndv==0：索引空/未知，保守走索引
    }

    /**
     * @brief 将 LogicalFilter 翻译为物理计划节点；若满足索引条件则生成 IndexScanPlan，否则生成 FilterPlan。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_filter(const LogicalFilter& f) {
        // IndexScan 选择：当 child 为 Scan 且 predicate 为单一比较（列 OP 字面量）且该列已建索引，
        // 则升级为 IndexScan（等值或范围）取代 SeqScan + Filter。
        if (f.child && f.child->kind == LogicalKind::Scan && f.predicate.kind == BoolExpr::Kind::Comparison &&
            f.predicate.cmp.has_value()) {
            const auto& scan = std::get<LogicalScan>(f.child->node);
            const auto& cmp = *f.predicate.cmp;
            // 提取 (列, 操作符, 字面量)，同时处理 col OP lit 与 lit OP col 两种写法。
            const ColumnRef* col = nullptr;
            const Literal* lit = nullptr;
            CompareOp op = cmp.op;
            if (auto* c = std::get_if<ColumnRef>(&cmp.lhs)) {
                col = c;
                lit = std::get_if<Literal>(&cmp.rhs);
            } else if (auto* c2 = std::get_if<ColumnRef>(&cmp.rhs)) {
                col = c2;
                lit = std::get_if<Literal>(&cmp.lhs);
                op = reverse_compare_op(op); // 字面量在左侧：反转方向（lit < col ≡ col > lit）
            }
            if (col && lit && scan.table && scan.table->indexed_columns().count(col->name) > 0) {
                if (op == CompareOp::Eq) {
                    // 代价决策：低基数列的等值非选择性，落回 SeqScan + Filter。
                    if (equality_worth_index(*scan.table, col->name)) {
                        return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name, lit->value);
                    }
                } else {
                    std::optional<Value> low, high;
                    bool low_inc = false, high_inc = false;
                    switch (op) {
                        case CompareOp::Gt:
                            low = lit->value;
                            break;
                        case CompareOp::Ge:
                            low = lit->value;
                            low_inc = true;
                            break;
                        case CompareOp::Lt:
                            high = lit->value;
                            break;
                        case CompareOp::Le:
                            high = lit->value;
                            high_inc = true;
                            break;
                        default:
                            break; // Ne/Like/IsNull 等不走索引
                    }
                    if (low.has_value() || high.has_value()) {
                        // 代价决策：非选择性范围落回 SeqScan + Filter（顺序流式扫描更优）。
                        if (range_worth_index(*scan.table, col->name, low, low_inc, high, high_inc)) {
                            return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name, std::move(low),
                                                                   low_inc, std::move(high), high_inc);
                        }
                    }
                }
            }
        }

        // BETWEEN low AND high → 双侧含等的范围 IndexScan（列已建索引且 low/high 为字面量）。
        if (f.child && f.child->kind == LogicalKind::Scan && f.predicate.kind == BoolExpr::Kind::Between &&
            f.predicate.between_expr.has_value() && !f.predicate.between_expr->negated) {
            const auto& scan = std::get<LogicalScan>(f.child->node);
            const auto& be = *f.predicate.between_expr;
            const auto* col = std::get_if<ColumnRef>(&be.expr);
            const auto* lo = std::get_if<Literal>(&be.low);
            const auto* hi = std::get_if<Literal>(&be.high);
            if (col && lo && hi && scan.table && scan.table->indexed_columns().count(col->name) > 0 &&
                range_worth_index(*scan.table, col->name, std::optional<Value>(lo->value), true,
                                  std::optional<Value>(hi->value), true)) {
                return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name,
                                                       std::optional<Value>(lo->value), true,
                                                       std::optional<Value>(hi->value), true);
            }
        }

        // col IN (v1, v2, ...) → 多个等值点查的并集 IndexScan（列已建索引且值均为字面量）。
        if (f.child && f.child->kind == LogicalKind::Scan && f.predicate.kind == BoolExpr::Kind::In &&
            f.predicate.in_expr.has_value() && !f.predicate.in_expr->negated) {
            const auto& scan = std::get<LogicalScan>(f.child->node);
            const auto& ie = *f.predicate.in_expr;
            const auto* col = std::get_if<ColumnRef>(&ie.expr);
            if (col && scan.table && !ie.values.empty() && scan.table->indexed_columns().count(col->name) > 0) {
                std::vector<Value> keys;
                keys.reserve(ie.values.size());
                bool all_lit = true;
                for (const auto& e: ie.values) {
                    if (const auto* lit = std::get_if<Literal>(&e)) {
                        keys.push_back(lit->value);
                    } else {
                        all_lit = false;
                        break;
                    }
                }
                if (all_lit) {
                    return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name, std::move(keys));
                }
            }
        }

        // 复合等值索引：合取 a=x AND b=y ... 且某复合索引的列集均被等值叶覆盖 → 复合 IndexScan。
        if (f.child && f.child->kind == LogicalKind::Scan && f.predicate.kind == BoolExpr::Kind::And) {
            const auto& scan = std::get<LogicalScan>(f.child->node);
            if (scan.table && !scan.table->composite_indexes().empty()) {
                // 展开 AND 树，收集等值叶 (列名→字面量)；记录是否存在非等值残差叶。
                std::unordered_map<std::string, Value> eqs;
                bool only_equalities = true;
                std::vector<const BoolExpr*> stack{ &f.predicate };
                while (!stack.empty()) {
                    const BoolExpr* e = stack.back();
                    stack.pop_back();
                    if (e->kind == BoolExpr::Kind::And) {
                        if (e->left)
                            stack.push_back(e->left.get());
                        if (e->right)
                            stack.push_back(e->right.get());
                        continue;
                    }
                    if (e->kind == BoolExpr::Kind::Comparison && e->cmp.has_value() && e->cmp->op == CompareOp::Eq) {
                        const ColumnRef* c = std::get_if<ColumnRef>(&e->cmp->lhs);
                        const Literal* l = std::get_if<Literal>(&e->cmp->rhs);
                        if (!c) {
                            c = std::get_if<ColumnRef>(&e->cmp->rhs);
                            l = std::get_if<Literal>(&e->cmp->lhs);
                        }
                        if (c && l) {
                            eqs.emplace(c->name, l->value);
                            continue;
                        }
                    }
                    only_equalities = false; // 存在非「列=字面量」的叶
                }
                // 选列集被完全覆盖且列数最多的复合索引。
                const std::string* best = nullptr;
                const std::vector<std::string>* best_cols = nullptr;
                for (const auto& [iname, cols]: scan.table->composite_indexes()) {
                    bool all = true;
                    for (const auto& cn: cols)
                        if (!eqs.count(cn)) {
                            all = false;
                            break;
                        }
                    if (all && (!best_cols || cols.size() > best_cols->size())) {
                        best = &iname;
                        best_cols = &cols;
                    }
                }
                if (best && best_cols) {
                    std::vector<Value> key;
                    key.reserve(best_cols->size());
                    for (const auto& cn: *best_cols)
                        key.push_back(eqs.at(cn));
                    auto idx_scan = std::make_unique<IndexScanPlan>(scan.table, scan.alias, *best, *best_cols,
                                                                   std::move(key));
                    // 合取恰好只由该索引列的等值构成 → 无残差，直接返回；否则叠加 Filter 复查全谓词。
                    if (only_equalities && eqs.size() == best_cols->size())
                        return idx_scan;
                    return std::make_unique<FilterPlan>(std::move(idx_scan), f.predicate);
                }
            }
        }

        auto child = visit(*f.child);
        return std::make_unique<FilterPlan>(std::move(child), f.predicate);
    }

    /**
     * @brief 将 LogicalProject 翻译为 ProjectPlan；若子节点为 Aggregate 则将列列表下沉到 AggregatePlan。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_project(const LogicalProject& p) {
        // 当 child 为 LogicalAggregate 时，把 Project 的列下沉到 AggregatePlan.projections，
        // 直接返回 AggregatePlan（避免 Executor 在 Project 层再次处理 AggregateExpr）。
        if (p.child && p.child->kind == LogicalKind::Aggregate) {
            const auto& agg = std::get<LogicalAggregate>(p.child->node);
            auto agg_plan = build_aggregate(agg);
            auto* hp = dynamic_cast<AggregatePlan*>(agg_plan.get());
            if (hp) {
                hp->projections = cols_to_items(p.columns);
            }
            return agg_plan;
        }
        // 当 Limit/Sort 节点夹在 Project 与 Aggregate 之间时（如 GROUP BY + LIMIT），
        // 穿透这些中间节点找到 LogicalAggregate，构建物理子树后向其注入 projections，
        // 避免空 projections 导致 "Column not found" 错误。
        {
            const LogicalPlan* inner = p.child.get();
            while (inner && (inner->kind == LogicalKind::Limit || inner->kind == LogicalKind::Sort)) {
                if (inner->kind == LogicalKind::Limit) {
                    inner = std::get<LogicalLimit>(inner->node).child.get();
                } else {
                    inner = std::get<LogicalSort>(inner->node).child.get();
                }
            }
            if (inner && inner->kind == LogicalKind::Aggregate) {
                auto child = p.child ? visit(*p.child) : nullptr;
                PlanNode* cursor = child.get();
                while (cursor) {
                    if (auto* ap = dynamic_cast<AggregatePlan*>(cursor)) {
                        ap->projections = cols_to_items(p.columns);
                        break;
                    }
                    if (auto* lp = dynamic_cast<LimitPlan*>(cursor)) {
                        cursor = lp->child.get();
                    } else if (auto* op = dynamic_cast<OrderByPlan*>(cursor)) {
                        cursor = op->child.get();
                    } else {
                        break;
                    }
                }
                return child;
            }
        }
        auto child = p.child ? visit(*p.child) : nullptr;
        return std::make_unique<ProjectPlan>(std::move(child), cols_to_items(p.columns), p.distinct);
    }

    /**
     * @brief 将 LogicalJoin 翻译为 HashJoinPlan / MergeJoinPlan / NestedLoopJoinPlan，
     *        按 ON 条件和子树排序状态选择最优物理实现。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_join(const LogicalJoin& j) {
        // P2: 等值 + 双侧表名清晰 → HashJoin（Inner/Left/Right/Full 均支持，执行器带 matched 标记 +
        //     收尾补 NULL）；否则 NestedLoop。
        // P3: 若等值且双侧 child 为 LogicalSort，且 sort key 顶层恰好分别等于左/右等值键 → MergeJoin。
        if ((j.join_type == JoinType::Inner || j.join_type == JoinType::Left || j.join_type == JoinType::Right ||
             j.join_type == JoinType::Full) &&
            j.on.has_value()) {
            std::unordered_set<std::string> ltabs, rtabs;
            if (j.left)
                collect_plan_tables(*j.left, ltabs);
            if (j.right)
                collect_plan_tables(*j.right, rtabs);
            ColumnRef lk, rk;
            if (try_extract_equi_keys(*j.on, ltabs, rtabs, lk, rk)) {
                // P3 检测：左右都是 Sort 且首个排序键匹配等值键
                bool merge_ok = false;
                if (j.left && j.left->kind == LogicalKind::Sort && j.right && j.right->kind == LogicalKind::Sort) {
                    const auto& ls = std::get<LogicalSort>(j.left->node);
                    const auto& rs = std::get<LogicalSort>(j.right->node);
                    auto first_col = [](const std::vector<LogicalSortKey>& ks) -> const ColumnRef* {
                        if (ks.empty())
                            return nullptr;
                        return std::get_if<ColumnRef>(&ks.front().expr);
                    };
                    const ColumnRef* lkey0 = first_col(ls.keys);
                    const ColumnRef* rkey0 = first_col(rs.keys);
                    if (lkey0 && rkey0 && lkey0->name == lk.name && rkey0->name == rk.name) {
                        merge_ok = true;
                    }
                }

                auto left = j.left ? visit(*j.left) : nullptr;
                auto right = j.right ? visit(*j.right) : nullptr;
                if (merge_ok) {
                    auto mj = std::make_unique<MergeJoinPlan>(std::move(left), std::move(right), std::move(lk),
                                                              std::move(rk), std::nullopt, j.join_type);
                    mj->left_sorted = true;
                    mj->right_sorted = true;
                    return mj;
                }
                return std::make_unique<HashJoinPlan>(std::move(left), std::move(right), std::move(lk), std::move(rk),
                                                      std::nullopt, j.join_type);
            }
        }

        if (!j.on.has_value()) {
            throw std::runtime_error("[PhysicalPlanner] Join without ON not supported");
        }
        auto left = j.left ? visit(*j.left) : nullptr;
        auto right = j.right ? visit(*j.right) : nullptr;
        return std::make_unique<NestedLoopJoinPlan>(std::move(left), std::move(right), *j.on, j.join_type);
    }

    /**
     * @brief 将 LogicalAggregate 翻译为 AggregatePlan，按 group_by 与子排序键匹配选择 Hash 或 Sort 策略。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_aggregate(const LogicalAggregate& a) {
        // T9.6.3: 决定 Hash vs Sort 策略 ——
        // 当 a.child 为 LogicalSort 且其 sort 键集合 == group_by 列集合时，可吸收排序为
        // SortAggregate（流式聚合，无需哈希表）。
        bool use_sort = false;
        const LogicalPlan* effective_child = a.child.get();
        if (a.child && std::holds_alternative<LogicalSort>(a.child->node) && !a.group_by.empty()) {
            const auto& ls = std::get<LogicalSort>(a.child->node);
            // 提取 group_by 列集合（仅 ColumnRef 形式参与匹配）
            std::vector<ColumnRef> gb_cols;
            gb_cols.reserve(a.group_by.size());
            bool all_col = true;
            for (const auto& e: a.group_by) {
                if (auto* c = std::get_if<ColumnRef>(&e))
                    gb_cols.push_back(*c);
                else {
                    all_col = false;
                    break;
                }
            }
            if (all_col && ls.keys.size() == gb_cols.size()) {
                auto col_eq = [](const ColumnRef& x, const ColumnRef& y) {
                    return x.name == y.name && (x.table.empty() || y.table.empty() || x.table == y.table);
                };
                auto contains = [&](const std::vector<ColumnRef>& v, const ColumnRef& c) {
                    for (const auto& e: v)
                        if (col_eq(e, c))
                            return true;
                    return false;
                };
                std::vector<ColumnRef> sort_cols;
                bool ok = true;
                for (const auto& k: ls.keys) {
                    if (auto* c = std::get_if<ColumnRef>(&k.expr))
                        sort_cols.push_back(*c);
                    else {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    bool same = true;
                    for (const auto& g: gb_cols)
                        if (!contains(sort_cols, g)) {
                            same = false;
                            break;
                        }
                    if (same)
                        for (const auto& s: sort_cols)
                            if (!contains(gb_cols, s)) {
                                same = false;
                                break;
                            }
                    if (same) {
                        use_sort = true;
                        // 吸收 sort 节点：直接拿 sort 的 child
                        effective_child = ls.child.get();
                    }
                }
            }
        }

        auto child = effective_child ? visit(*effective_child) : nullptr;

        std::vector<ColumnRef> group;
        group.reserve(a.group_by.size());
        for (const auto& e: a.group_by) {
            if (auto* c = std::get_if<ColumnRef>(&e))
                group.push_back(*c);
            else
                throw std::runtime_error("[PhysicalPlanner] GROUP BY expression must be a column");
        }

        std::vector<AggregateExpr> aggs;
        aggs.reserve(a.aggregates.size());
        for (const auto& it: a.aggregates) {
            AggregateExpr ag;
            ag.func = it.func;
            if (it.arg.has_value()) {
                if (auto* c = std::get_if<ColumnRef>(&*it.arg))
                    ag.arg = *c;
            }
            aggs.push_back(std::move(ag));
        }

        std::vector<SelectStmt::SelectItem> proj;
        return std::make_unique<AggregatePlan>(
                std::move(child), std::move(group), std::move(aggs), std::move(proj), a.having,
                use_sort ? AggregatePlan::Strategy::Sort : AggregatePlan::Strategy::Hash);
    }

    /**
     * @brief 将 LogicalSort 翻译为 OrderByPlan。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_sort(const LogicalSort& s) {
        auto child = s.child ? visit(*s.child) : nullptr;
        std::vector<SelectStmt::OrderByItem> items;
        items.reserve(s.keys.size());
        for (const auto& k: s.keys) {
            SelectStmt::OrderByItem ob;
            ob.key = k.expr;
            ob.asc = k.ascending;
            items.push_back(std::move(ob));
        }
        return std::make_unique<OrderByPlan>(std::move(child), std::move(items));
    }

    /**
     * @brief 将 LogicalLimit 翻译为 LimitPlan；若 child 为 OrderByPlan 则下推 Top-N 提示。
     *
     * 执行器对带 limit 的 OrderByPlan 用 K 元堆（K=limit+offset）只保前 K 小，
     * 代替全量物化 + 全排序；外层 LimitPlan 保留负责 offset 裁剪。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_limit(const LogicalLimit& l) {
        auto child = l.child ? visit(*l.child) : nullptr;
        std::optional<int64_t> lim, off;
        if (l.limit.has_value())
            lim = static_cast<int64_t>(*l.limit);
        if (l.offset.has_value())
            off = static_cast<int64_t>(*l.offset);
        if (lim.has_value()) {
            if (auto* ob = dynamic_cast<OrderByPlan*>(child.get())) {
                ob->limit = lim;
                ob->offset = off;
            }
        }
        return std::make_unique<LimitPlan>(std::move(child), lim, off);
    }

    /**
     * @brief 将 LogicalDML 翻译为 InsertPlan / UpdatePlan / DeletePlan。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_dml(const LogicalDML& d) {
        switch (d.kind) {
            case LogicalDML::Kind::Insert: {
                if (!d.table)
                    throw std::runtime_error("[PhysicalPlanner] DML.Insert missing table");
                std::vector<std::size_t> indexes;
                if (d.columns.empty()) {
                    indexes.resize(d.table->columns().size());
                    for (std::size_t i = 0; i < indexes.size(); ++i)
                        indexes[i] = i;
                } else {
                    for (const auto& c: d.columns)
                        indexes.push_back(resolve_column_index(*d.table, c));
                }
                if (!d.source || d.source->kind != LogicalKind::Values) {
                    throw std::runtime_error("[PhysicalPlanner] DML.Insert source must be Values");
                }
                const auto& vals = std::get<LogicalValues>(d.source->node);
                std::vector<std::vector<Value>> all_rows;
                all_rows.reserve(vals.rows.size());
                for (const auto& row: vals.rows) {
                    if (row.size() != indexes.size()) {
                        throw std::runtime_error("[PhysicalPlanner] INSERT columns/values size mismatch");
                    }
                    std::vector<Value> vs;
                    vs.reserve(row.size());
                    for (const auto& e: row)
                        vs.push_back(materialize_constant(e));
                    all_rows.push_back(std::move(vs));
                }
                return std::make_unique<InsertPlan>(d.table, std::move(indexes), std::move(all_rows));
            }
            case LogicalDML::Kind::Update: {
                if (!d.table)
                    throw std::runtime_error("[PhysicalPlanner] DML.Update missing table");
                std::vector<UpdatePlan::Assignment> assigns;
                if (d.columns.size() != d.set_exprs.size()) {
                    throw std::runtime_error("[PhysicalPlanner] DML.Update columns/exprs mismatch");
                }
                assigns.reserve(d.columns.size());
                for (std::size_t i = 0; i < d.columns.size(); ++i) {
                    assigns.push_back(
                            UpdatePlan::Assignment{ resolve_column_index(*d.table, d.columns[i]), d.set_exprs[i] });
                }
                return std::make_unique<UpdatePlan>(d.table, std::move(assigns), d.where);
            }
            case LogicalDML::Kind::Delete: {
                if (!d.table)
                    throw std::runtime_error("[PhysicalPlanner] DML.Delete missing table");
                return std::make_unique<DeletePlan>(d.table, d.where);
            }
        }
        throw std::runtime_error("[PhysicalPlanner] Unknown DML kind");
    }

    /**
     * @brief 把 LogicalDDL 翻译为对应的 DDL PlanNode。
     *
     * LogicalDDL 内部封装的是原始 AST Statement；按 variant 分发到
     * CreateTablePlan / CreateIndexPlan / DropTablePlan / DropIndexPlan。
     *
     * @throws std::runtime_error 当 PhysicalPlanner 不持有 Catalog/StorageEngine
     *                            或 DDL 形态未识别。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_ddl(const LogicalDDL& d) {
        if (!catalog_) {
            throw std::runtime_error("[PhysicalPlanner] DDL requires Catalog (use full constructor)");
        }
        return std::visit(
                [this](const auto& s) -> std::unique_ptr<PlanNode> {
                    using T = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<T, CreateStmt>) {
                        if (!storage_) {
                            throw std::runtime_error("[PhysicalPlanner] CREATE TABLE requires StorageEngine");
                        }
                        if (catalog_->lookup(s.table)) {
                            throw std::runtime_error("[PhysicalPlanner] Table already exists: " + s.table);
                        }
                        if (storage_->table_exists(s.table)) {
                            throw std::runtime_error("Table already exists: " + s.table);
                        }
                        std::vector<Column> cols;
                        cols.reserve(s.columns.size());
                        for (const auto& c: s.columns) {
                            Column col{ s.table, c.name, c.type };
                            col.not_null = c.not_null || c.primary_key; // PRIMARY KEY 隐含 NOT NULL
                            col.primary_key = c.primary_key;
                            cols.push_back(col);
                        }
                        return std::make_unique<CreateTablePlan>(s.table, std::move(cols), catalog_, storage_);
                    } else if constexpr (std::is_same_v<T, CreateIndexStmt>) {
                        auto table = catalog_->lookup(s.table);
                        if (!table) {
                            throw std::runtime_error("[PhysicalPlanner] Unknown table: " + s.table);
                        }
                        for (const auto& ic: s.columns) {
                            bool found = false;
                            for (const auto& c: table->columns()) {
                                if (c.name == ic) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                throw std::runtime_error("[PhysicalPlanner] Unknown column for index: " + ic);
                            }
                        }
                        return std::make_unique<CreateIndexPlan>(std::move(table), s.index_name, s.columns);
                    } else if constexpr (std::is_same_v<T, DropTableStmt>) {
                        return std::make_unique<DropTablePlan>(s.table, s.if_exists, catalog_, storage_);
                    } else if constexpr (std::is_same_v<T, DropIndexStmt>) {
                        std::shared_ptr<Table> table;
                        if (!s.table.empty()) {
                            table = catalog_->lookup(s.table);
                            if (!table && !s.if_exists) {
                                throw std::runtime_error("[PhysicalPlanner] Unknown table: " + s.table);
                            }
                        }
                        return std::make_unique<DropIndexPlan>(std::move(table), s.index_name, s.if_exists);
                    } else {
                        throw std::runtime_error("[PhysicalPlanner] DDL form not handled by physical planner");
                    }
                },
                d.original);
    }

} // namespace corodb::opt
