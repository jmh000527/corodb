// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file physical_planner.cpp
// @brief 物理查询计划生成器的实现。

#include "corodb/optimizer/physical/physical_planner.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "corodb/common/config.h"
#include "corodb/optimizer/cost/cost_model.h"
#include "corodb/optimizer/stats/selectivity.h"
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

        // ------------------------------------------------------------------
        // 基数 + 代价标注（T1.7 + Phase 2）：自底向上填 estimated_rows 与 cost
        // ------------------------------------------------------------------

        /** @brief 子树的单表来源（Filter/Project 下钻到扫描）；多表或未知返回 nullptr。 */
        const Table* driving_table(const PlanNode* n) {
            if (!n)
                return nullptr;
            if (const auto* s = dynamic_cast<const SeqScanPlan*>(n))
                return s->table.get();
            if (const auto* s = dynamic_cast<const IndexScanPlan*>(n))
                return s->table.get();
            if (const auto* f = dynamic_cast<const FilterPlan*>(n))
                return driving_table(f->child.get());
            if (const auto* p = dynamic_cast<const ProjectPlan*>(n))
                return driving_table(p->child.get());
            return nullptr;
        }

        /** @brief IndexScan 索引条件选择率（等值/范围/IN/复合）。 */
        double index_scan_selectivity(const IndexScanPlan& idx) {
            const Table& t = *idx.table;
            const ColumnStats* cs = t.column_stats(idx.column);
            if (idx.is_composite) {
                double sel = 1.0;
                for (std::size_t i = 0; i < idx.composite_columns.size() && i < idx.composite_key.size(); ++i) {
                    sel *= opt::SelectivityEstimator::selectivity_eq(t.column_stats(idx.composite_columns[i]),
                                                                    idx.composite_key[i]);
                }
                return sel;
            }
            if (idx.is_in)
                return opt::SelectivityEstimator::selectivity_in_list(cs, idx.in_keys);
            if (idx.is_range)
                return opt::SelectivityEstimator::selectivity_range(cs, idx.low, idx.low_inclusive, idx.high,
                                                                   idx.high_inclusive);
            return opt::SelectivityEstimator::selectivity_eq(cs, idx.key);
        }

        /** @brief 连接输出估计：|L|×|R|/max(NDV_left, NDV_right)，NDV 不可解析回退 max(L,R)。 */
        std::size_t estimate_join_rows(std::size_t l, std::size_t r, const Table* lt, const Table* rt,
                                       const ColumnRef& lkey, const ColumnRef& rkey) {
            std::size_t ndv = 0;
            if (lt && !lkey.name.empty())
                ndv = std::max(ndv, lt->index_distinct_count(lkey.name, 10000));
            if (rt && !rkey.name.empty())
                ndv = std::max(ndv, rt->index_distinct_count(rkey.name, 10000));
            if (ndv > 0)
                return std::max<std::size_t>((l * r) / ndv, 1);
            return std::max<std::size_t>(std::max(l, r), 1);
        }

        /** @brief 分组数估计：min(输入行数, 组键 NDV 乘积)；NDV 不可解析回退 child/10；全局聚合为 1。 */
        std::size_t estimate_group_count(const PlanNode* child, const std::vector<ColumnRef>& group_by) {
            const std::size_t child_rows = child ? child->estimated_rows : 0;
            if (group_by.empty())
                return 1;
            double g = 1.0;
            if (const Table* t = driving_table(child)) {
                for (const auto& grp: group_by) {
                    const double ndv = static_cast<double>(t->index_distinct_count(grp.name, 10000));
                    if (ndv <= 0) {
                        g = 0.0;
                        break;
                    }
                    g *= ndv;
                    if (g > 1e15)
                        break;
                }
            } else {
                g = 0.0;
            }
            return g > 0.0
                       ? std::max<std::size_t>(1, static_cast<std::size_t>(
                                                          std::min<double>(g, static_cast<double>(child_rows))))
                       : std::max<std::size_t>(child_rows / 10, 1);
        }

        /** @brief 递归标注：先子后父，填 estimated_rows 与 startup/total_cost。 */
        void annotate(PlanNode* node) {
            if (!node)
                return;
            const opt::CostModel cm{ opt::CostModel::Params::from_config() };
            const auto rows_of = [](const PlanNode* n) {
                return n ? static_cast<double>(n->estimated_rows) : 0.0;
            };
            const auto total_of = [](const PlanNode* n) {
                return n ? n->total_cost : 0.0;
            };
            const auto startup_of = [](const PlanNode* n) {
                return n ? n->startup_cost : 0.0;
            };

            if (auto* seq = dynamic_cast<SeqScanPlan*>(node)) {
                seq->estimated_rows = seq->table ? seq->table->estimated_row_count() : 0;
                const Cost c = cm.seq_scan(static_cast<double>(seq->estimated_rows));
                seq->startup_cost = c.startup;
                seq->total_cost = c.total;
                return;
            }
            if (auto* idx = dynamic_cast<IndexScanPlan*>(node)) {
                const std::size_t base = idx->table ? idx->table->estimated_row_count() : 0;
                const double sel = idx->table ? index_scan_selectivity(*idx) : 0.0;
                const double out = static_cast<double>(base) * sel;
                idx->estimated_rows = base == 0 ? 0 : std::max<std::size_t>(1, static_cast<std::size_t>(out));
                const double corr = idx->table ? [&] {
                    if (const ColumnStats* cs = idx->table->column_stats(idx->column))
                        return cs->correlation;
                    return 0.0;
                }()
                                               : 0.0;
                const Cost c = cm.index_scan(out, static_cast<double>(base), corr);
                idx->startup_cost = c.startup;
                idx->total_cost = c.total;
                return;
            }
            if (auto* fil = dynamic_cast<FilterPlan*>(node)) {
                annotate(fil->child.get());
                double sel = 1.0 / 3.0; // 无统计回退（与 R5 旧模型一致）
                if (const Table* t = driving_table(fil->child.get()))
                    sel = opt::SelectivityEstimator::estimate_filter_selectivity(*t, fil->predicate);
                const double child_rows = rows_of(fil->child.get());
                fil->estimated_rows = fil->child
                                              ? std::max<std::size_t>(
                                                        1, static_cast<std::size_t>(child_rows * sel))
                                              : 0;
                const Cost c = cm.filter(total_of(fil->child.get()), child_rows);
                fil->startup_cost = c.startup;
                fil->total_cost = c.total;
                return;
            }
            if (auto* proj = dynamic_cast<ProjectPlan*>(node)) {
                annotate(proj->child.get());
                proj->estimated_rows = proj->child ? proj->child->estimated_rows : 0;
                proj->startup_cost = startup_of(proj->child.get());
                proj->total_cost = total_of(proj->child.get());
                return;
            }
            if (auto* un = dynamic_cast<UnionPlan*>(node)) {
                double total = 0.0;
                std::size_t rows = 0;
                for (auto& c: un->children) {
                    annotate(c.get());
                    total += total_of(c.get());
                    rows += c ? c->estimated_rows : 0;
                }
                un->estimated_rows = rows;
                const Cost c = cm.union_(total);
                un->startup_cost = c.startup;
                un->total_cost = c.total;
                return;
            }
            if (auto* hash = dynamic_cast<HashJoinPlan*>(node)) {
                annotate(hash->left.get());
                annotate(hash->right.get());
                hash->estimated_rows =
                        estimate_join_rows(hash->left ? hash->left->estimated_rows : 1,
                                           hash->right ? hash->right->estimated_rows : 1,
                                           driving_table(hash->left.get()), driving_table(hash->right.get()),
                                           hash->left_key, hash->right_key);
                const Cost c = cm.hash_join(total_of(hash->left.get()), total_of(hash->right.get()),
                                            rows_of(hash->left.get()), rows_of(hash->right.get()),
                                            static_cast<double>(hash->estimated_rows));
                hash->startup_cost = c.startup;
                hash->total_cost = c.total;
                return;
            }
            if (auto* merge = dynamic_cast<MergeJoinPlan*>(node)) {
                annotate(merge->left.get());
                annotate(merge->right.get());
                merge->estimated_rows =
                        estimate_join_rows(merge->left ? merge->left->estimated_rows : 1,
                                           merge->right ? merge->right->estimated_rows : 1,
                                           driving_table(merge->left.get()), driving_table(merge->right.get()),
                                           merge->left_key, merge->right_key);
                const bool l_sorted = merge->left_sorted;
                const bool r_sorted = merge->right_sorted;
                const Cost c =
                        cm.merge_join(total_of(merge->left.get()), total_of(merge->right.get()),
                                      rows_of(merge->left.get()), rows_of(merge->right.get()),
                                      static_cast<double>(merge->estimated_rows), !l_sorted, !r_sorted);
                merge->startup_cost = c.startup;
                merge->total_cost = c.total;
                return;
            }
            if (auto* nl = dynamic_cast<NestedLoopJoinPlan*>(node)) {
                annotate(nl->left.get());
                annotate(nl->right.get());
                // 非等值连接无 NDV 模型：保守取 max(L,R)。
                nl->estimated_rows = std::max<std::size_t>(
                        std::max(nl->left ? nl->left->estimated_rows : 0,
                                 nl->right ? nl->right->estimated_rows : 0),
                        1);
                const Cost c = cm.nested_loop(total_of(nl->left.get()), total_of(nl->right.get()),
                                              rows_of(nl->left.get()),
                                              static_cast<double>(nl->estimated_rows));
                nl->startup_cost = c.startup;
                nl->total_cost = c.total;
                return;
            }
            if (auto* agg = dynamic_cast<AggregatePlan*>(node)) {
                annotate(agg->child.get());
                const std::size_t child_rows = agg->child ? agg->child->estimated_rows : 0;
                agg->estimated_rows = estimate_group_count(agg->child.get(), agg->group_by);
                const double crows = static_cast<double>(child_rows);
                const Cost c =
                        agg->strategy == AggregatePlan::Strategy::Sort
                                ? cm.sort_aggregate(total_of(agg->child.get()), crows)
                                : cm.hash_aggregate(total_of(agg->child.get()), crows,
                                                    static_cast<double>(agg->estimated_rows));
                agg->startup_cost = c.startup;
                agg->total_cost = c.total;
                return;
            }
            if (auto* ord = dynamic_cast<OrderByPlan*>(node)) {
                annotate(ord->child.get());
                ord->estimated_rows = ord->child ? ord->child->estimated_rows : 0;
                const double crows = rows_of(ord->child.get());
                if (ord->limit.has_value()) {
                    // Top-N：K 元堆只保前 K 小。
                    const double k = static_cast<double>(*ord->limit) +
                                     static_cast<double>(ord->offset.value_or(0));
                    const Cost c = cm.top_n_sort(total_of(ord->child.get()), crows, k);
                    ord->startup_cost = c.startup;
                    ord->total_cost = c.total;
                } else {
                    const Cost c = cm.sort(total_of(ord->child.get()), crows, crows);
                    ord->startup_cost = c.startup;
                    ord->total_cost = c.total;
                }
                return;
            }
            if (auto* lim = dynamic_cast<LimitPlan*>(node)) {
                annotate(lim->child.get());
                const std::size_t child_rows = lim->child ? lim->child->estimated_rows : 0;
                std::size_t rows = child_rows;
                if (lim->offset.has_value())
                    rows = rows > static_cast<std::size_t>(*lim->offset)
                               ? rows - static_cast<std::size_t>(*lim->offset)
                               : 0;
                if (lim->limit.has_value())
                    rows = std::min<std::size_t>(rows, static_cast<std::size_t>(*lim->limit));
                lim->estimated_rows = rows;
                const Cost c = cm.limit(total_of(lim->child.get()));
                lim->startup_cost = c.startup;
                lim->total_cost = c.total;
                return;
            }
            // DML/DDL 计划：估计无意义，保持 0。
        }
    } // namespace

    /**
     * @brief 将逻辑计划树翻译为可执行的物理计划树。
     * @param lp 逻辑计划根节点。
     * @return 物理计划根节点。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::plan(const LogicalPlan& lp) {
        // visit() 逐节点标注估计行数与代价（自底向上），build_* 决策可直接读取子树代价。
        return visit(lp);
    }

    /**
     * @brief 按节点类型分发到对应的 build_* 方法；构建后立即标注估计。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::visit(const LogicalPlan& lp) {
        std::unique_ptr<PlanNode> node = std::visit(
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
        // T1.7 + Phase 2：自底向上填 estimated_rows 与 cost（EXPLAIN 展示 + 上层代价决策输入）。
        annotate(node.get());
        return node;
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
     *
     * 访问路径决策（Phase 2，G1）：IndexScan 与 SeqScan+Filter 双候选按总代价比较；
     * 小表（<128 行）短路走索引；代价模型关闭（[optimizer].cost_model=false）或表无统计时
     * 回退旧阈值启发式（OPT-2/3/6），保证估计缺失时的行为不变。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_filter(const LogicalFilter& f) {
        // 代价比较：IndexScan 与 SeqScan+Filter 双候选（需子扫描已标注代价）。
        // 返回 nullopt = 应回退旧阈值启发式。
        auto index_beats_seq_scan = [&](const Table& t, double sel, double corr) -> std::optional<bool> {
            constexpr std::size_t kSmallTable = 128;
            if (t.estimated_row_count() < kSmallTable)
                return true;
            if (!Config::instance().cost_model_enabled() || !t.has_stats())
                return std::nullopt;
            auto seq_node = visit(*f.child); // 已标注的 SeqScan 候选
            const opt::CostModel cm{ opt::CostModel::Params::from_config() };
            const double base = static_cast<double>(seq_node->estimated_rows);
            const Cost idx = cm.index_scan(base * sel, base, corr);
            const Cost seq = cm.filter(seq_node->total_cost, base);
            return idx < seq;
        };
        // 列统计的物理相关性（无统计为 0 → 全随机读计价）。
        auto col_correlation = [](const Table& t, const std::string& col) -> double {
            if (const ColumnStats* cs = t.column_stats(col))
                return cs->correlation;
            return 0.0;
        };

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
                    const double sel = opt::SelectivityEstimator::selectivity_eq(
                            scan.table->column_stats(col->name), lit->value);
                    auto decision =
                            index_beats_seq_scan(*scan.table, sel, col_correlation(*scan.table, col->name));
                    // 代价决策：低基数列的等值非选择性，落回 SeqScan + Filter。
                    const bool use_index =
                            decision.has_value() ? *decision : equality_worth_index(*scan.table, col->name);
                    if (use_index) {
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
                        const double sel = opt::SelectivityEstimator::selectivity_range(
                                scan.table->column_stats(col->name), low, low_inc, high, high_inc);
                        auto decision =
                                index_beats_seq_scan(*scan.table, sel, col_correlation(*scan.table, col->name));
                        // 代价决策：非选择性范围落回 SeqScan + Filter（顺序流式扫描更优）。
                        const bool use_index = decision.has_value()
                                                   ? *decision
                                                   : range_worth_index(*scan.table, col->name, low, low_inc, high,
                                                                       high_inc);
                        if (use_index) {
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
            if (col && lo && hi && scan.table && scan.table->indexed_columns().count(col->name) > 0) {
                const double sel = opt::SelectivityEstimator::selectivity_between(
                        scan.table->column_stats(col->name), lo->value, hi->value);
                auto decision = index_beats_seq_scan(*scan.table, sel, col_correlation(*scan.table, col->name));
                const bool use_index = decision.has_value()
                                           ? *decision
                                           : range_worth_index(*scan.table, col->name,
                                                               std::optional<Value>(lo->value), true,
                                                               std::optional<Value>(hi->value), true);
                if (use_index) {
                    return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name,
                                                           std::optional<Value>(lo->value), true,
                                                           std::optional<Value>(hi->value), true);
                }
            }
        }

        // col IN (v1, v2, ...) → 多个等值点查的并集 IndexScan（列已建索引且值均为字面量）。
        // 旧行为无代价检查（恒走索引）；有统计时代价比较，无统计回退恒走索引。
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
                    const double sel = opt::SelectivityEstimator::selectivity_in_list(
                            scan.table->column_stats(col->name), keys);
                    auto decision =
                            index_beats_seq_scan(*scan.table, sel, col_correlation(*scan.table, col->name));
                    if (!decision.has_value() || *decision) {
                        return std::make_unique<IndexScanPlan>(scan.table, scan.alias, col->name, std::move(keys));
                    }
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
     * @brief 将 LogicalJoin 翻译为 HashJoinPlan / MergeJoinPlan / NestedLoopJoinPlan。
     *
     * 算法选择（Phase 2，G1）：等值连接时对 Hash / NL（输入已有序时还有 Merge）
     * 按总代价比较；代价模型关闭时等值默认 Hash（旧行为），非等值恒 NL。
     */
    std::unique_ptr<PlanNode> PhysicalPlanner::build_join(const LogicalJoin& j) {
        auto build_children = [&](std::unique_ptr<PlanNode>& left, std::unique_ptr<PlanNode>& right) {
            left = j.left ? visit(*j.left) : nullptr;
            right = j.right ? visit(*j.right) : nullptr;
        };

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
                // P3 检测：左右都是 Sort 且首个排序键匹配等值键（输入已按连接键有序）。
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

                std::unique_ptr<PlanNode> left, right;
                build_children(left, right);

                const std::size_t lrows = left ? left->estimated_rows : 1;
                const std::size_t rrows = right ? right->estimated_rows : 1;
                const std::size_t out_rows = estimate_join_rows(
                        lrows, rrows, driving_table(left.get()), driving_table(right.get()), lk, rk);
                const opt::CostModel cm{ opt::CostModel::Params::from_config() };
                const double lt = left ? left->total_cost : 0.0;
                const double rt = right ? right->total_cost : 0.0;

                if (Config::instance().cost_model_enabled()) {
                    // MergeJoin 候选：仅当输入已按连接键有序（免排序）。
                    // 只比 total：同总代价时归并免建哈希表，优先归并。
                    if (merge_ok) {
                        const Cost merge =
                                cm.merge_join(lt, rt, static_cast<double>(lrows), static_cast<double>(rrows),
                                              static_cast<double>(out_rows), false, false);
                        const Cost hash = cm.hash_join(lt, rt, static_cast<double>(lrows),
                                                       static_cast<double>(rrows),
                                                       static_cast<double>(out_rows));
                        if (merge.total <= hash.total) {
                            auto mj = std::make_unique<MergeJoinPlan>(std::move(left), std::move(right),
                                                                      std::move(lk), std::move(rk), std::nullopt,
                                                                      j.join_type);
                            mj->left_sorted = true;
                            mj->right_sorted = true;
                            return mj;
                        }
                        merge_ok = false; // Hash 严格更优：落回 Hash/NL 比较
                    }
                    const Cost hash = cm.hash_join(lt, rt, static_cast<double>(lrows), static_cast<double>(rrows),
                                                   static_cast<double>(out_rows));
                    const Cost nl = cm.nested_loop(lt, rt, static_cast<double>(lrows),
                                                   static_cast<double>(out_rows));
                    // 平局归 Hash：等值连接哈希为线性伸缩的默认选择。
                    if (nl < hash) {
                        return std::make_unique<NestedLoopJoinPlan>(std::move(left), std::move(right), *j.on,
                                                                    j.join_type);
                    }
                    return std::make_unique<HashJoinPlan>(std::move(left), std::move(right), std::move(lk),
                                                          std::move(rk), std::nullopt, j.join_type);
                }

                // 代价模型关闭：旧启发式（等值 → Merge 若有序否则 Hash）。
                if (merge_ok) {
                    auto mj = std::make_unique<MergeJoinPlan>(std::move(left), std::move(right), std::move(lk),
                                                              std::move(rk), std::nullopt, j.join_type);
                    mj->left_sorted = true;
                    mj->right_sorted = true;
                    return mj;
                }
                return std::make_unique<HashJoinPlan>(std::move(left), std::move(right), std::move(lk),
                                                      std::move(rk), std::nullopt, j.join_type);
            }
        }

        if (!j.on.has_value()) {
            throw std::runtime_error("[PhysicalPlanner] Join without ON not supported");
        }
        std::unique_ptr<PlanNode> left, right;
        build_children(left, right);
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

        // 代价决策（Phase 2，G1）：输入已按组键有序时，Sort（流式）与 Hash 按代价比较；
        // 平局保持 Sort（不换将），代价模型关闭时维持旧行为（有序候选 → Sort）。
        if (use_sort && Config::instance().cost_model_enabled()) {
            const opt::CostModel cm{ opt::CostModel::Params::from_config() };
            const double crows = static_cast<double>(child ? child->estimated_rows : 0);
            const double ctotal = child ? child->total_cost : 0.0;
            const double groups = static_cast<double>(estimate_group_count(child.get(), group));
            const Cost sort_c = cm.sort_aggregate(ctotal, crows);
            const Cost hash_c = cm.hash_aggregate(ctotal, crows, groups);
            if (hash_c < sort_c)
                use_sort = false;
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
