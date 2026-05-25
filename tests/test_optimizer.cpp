/**
 * @file test_optimizer.cpp
 * @brief Optimizer / RBO 单元测试（T7.3 起步）
 *
 * 当前覆盖：
 *   - LogicalPlan 数据结构基本可构造、可 clone、可 to_string
 *   - R3 ConstantFolding：常量算术 / 字符串拼接 / NULL 传播
 *   - RuleSet 固定点：无变化时 iterations 收敛、不死循环
 */

#include <gtest/gtest.h>

#include "corodb/optimizer/logical/logical_planner.h"
#include "corodb/optimizer/physical/physical_planner.h"
#include "corodb/optimizer/logical/rule.h"
#include "corodb/plan/logical_plan.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/table.h"

using namespace corodb;
using namespace corodb::opt;

namespace {
    // helper: 构造 BinaryExpr(op, lhs_int, rhs_int)
    Expression bin_int(BinaryExpr::Op op, int64_t a, int64_t b) {
        BinaryExpr e;
        e.op = op;
        e.lhs = Literal{ Value{ a } };
        e.rhs = Literal{ Value{ b } };
        return std::make_shared<BinaryExpr>(std::move(e));
    }

    Expression bin_str(BinaryExpr::Op op, std::string a, std::string b) {
        BinaryExpr e;
        e.op = op;
        e.lhs = Literal{ Value{ std::move(a) } };
        e.rhs = Literal{ Value{ std::move(b) } };
        return std::make_shared<BinaryExpr>(std::move(e));
    }

    BoolExpr cmp_eq(Expression lhs, Expression rhs) {
        BoolExpr b;
        b.kind = BoolExpr::Kind::Comparison;
        Comparison c;
        c.lhs = std::move(lhs);
        c.op = CompareOp::Eq;
        c.rhs = std::move(rhs);
        b.cmp = std::move(c);
        return b;
    }

    LogicalPlanPtr make_dummy_filter(BoolExpr pred) {
        // 不能没有 child；用 LogicalValues 充当 source
        auto values = LogicalPlan::make(LogicalKind::Values, LogicalValues{ {} });
        return LogicalPlan::make_filter(std::move(pred), std::move(values));
    }
} // namespace

TEST(LogicalPlan, CloneIsDeep) {
    auto v = LogicalPlan::make(LogicalKind::Values, LogicalValues{});
    auto orig = LogicalPlan::make_filter(cmp_eq(Literal{ Value{ int64_t{ 1 } } }, Literal{ Value{ int64_t{ 1 } } }),
                                         std::move(v));
    auto copy = clone(*orig);
    ASSERT_NE(orig.get(), copy.get());
    EXPECT_EQ(copy->kind, LogicalKind::Filter);
    auto& fcopy = std::get<LogicalFilter>(copy->node);
    ASSERT_NE(fcopy.child.get(), nullptr);
    EXPECT_EQ(fcopy.child->kind, LogicalKind::Values);
}

TEST(LogicalPlan, ToStringNotEmpty) {
    auto v = LogicalPlan::make(LogicalKind::Values, LogicalValues{});
    auto p = LogicalPlan::make_filter(cmp_eq(Literal{ Value{ int64_t{ 1 } } }, Literal{ Value{ int64_t{ 1 } } }),
                                      std::move(v));
    auto s = to_string(*p);
    EXPECT_NE(s.find("Filter"), std::string::npos);
    EXPECT_NE(s.find("Values"), std::string::npos);
}

TEST(R3ConstantFolding, FoldsArithmeticInFilter) {
    // WHERE 1 + 2 = 3 → folded to a comparison whose lhs becomes Literal(3)
    auto pred = cmp_eq(bin_int(BinaryExpr::Op::Add, 1, 2), Literal{ Value{ int64_t{ 3 } } });
    auto plan = make_dummy_filter(std::move(pred));

    RuleSet rs = make_default_rules();
    RuleSet::ApplyStats stats;
    plan = rs.apply(std::move(plan), &stats);
    ASSERT_TRUE(plan);
    EXPECT_GT(stats.total_rewrites, 0) << "R3 should have folded 1+2 → 3";

    // 检查 lhs 现在是 Literal(3)
    const auto& filter = std::get<LogicalFilter>(plan->node);
    ASSERT_TRUE(filter.predicate.cmp.has_value());
    const auto& lhs = filter.predicate.cmp->lhs;
    ASSERT_TRUE(std::holds_alternative<Literal>(lhs));
    auto v = std::get<Literal>(lhs).value;
    ASSERT_TRUE(std::holds_alternative<int64_t>(v));
    EXPECT_EQ(std::get<int64_t>(v), 3);
}

TEST(R3ConstantFolding, FoldsStringConcat) {
    auto pred = cmp_eq(bin_str(BinaryExpr::Op::Concat, "foo", "bar"), Literal{ Value{ std::string{ "foobar" } } });
    auto plan = make_dummy_filter(std::move(pred));
    auto p2 = make_default_rules().apply(std::move(plan));
    const auto& filter = std::get<LogicalFilter>(p2->node);
    const auto& lhs = filter.predicate.cmp->lhs;
    ASSERT_TRUE(std::holds_alternative<Literal>(lhs));
    EXPECT_EQ(std::get<std::string>(std::get<Literal>(lhs).value), "foobar");
}

TEST(R3ConstantFolding, NullPropagatesThroughArithmetic) {
    BinaryExpr inner;
    inner.op = BinaryExpr::Op::Add;
    inner.lhs = Literal{ Value{ NullValue{} } };
    inner.rhs = Literal{ Value{ int64_t{ 1 } } };
    auto pred = cmp_eq(std::make_shared<BinaryExpr>(std::move(inner)), Literal{ Value{ NullValue{} } });
    auto plan = make_dummy_filter(std::move(pred));
    auto p2 = make_default_rules().apply(std::move(plan));
    const auto& filter = std::get<LogicalFilter>(p2->node);
    const auto& lhs = filter.predicate.cmp->lhs;
    ASSERT_TRUE(std::holds_alternative<Literal>(lhs));
    EXPECT_TRUE(std::holds_alternative<NullValue>(std::get<Literal>(lhs).value));
}

TEST(R3ConstantFolding, NoChangeForNonConstExpr) {
    // 列引用不应被折叠
    ColumnRef col;
    col.name = "x";
    BoolExpr pred = cmp_eq(col, Literal{ Value{ int64_t{ 1 } } });
    auto plan = make_dummy_filter(std::move(pred));

    RuleSet rs = make_default_rules();
    RuleSet::ApplyStats stats;
    plan = rs.apply(std::move(plan), &stats);
    EXPECT_EQ(stats.total_rewrites, 0);
    EXPECT_EQ(stats.iterations, 1); // 第一轮无改动即停止
}

TEST(RuleSet, FixpointConvergesOnIdempotentPlan) {
    // 应用规则到一个已经折叠完的 plan，不应产生重复改写
    auto pred = cmp_eq(Literal{ Value{ int64_t{ 42 } } }, Literal{ Value{ int64_t{ 42 } } });
    auto plan = make_dummy_filter(std::move(pred));

    RuleSet rs = make_default_rules();
    RuleSet::ApplyStats stats;
    plan = rs.apply(std::move(plan), &stats);
    EXPECT_EQ(stats.total_rewrites, 0);
    EXPECT_LE(stats.iterations, 2);
}

// ===== T4.2: LogicalPlanner =====
namespace {
    Catalog make_catalog_with_users_orders() {
        Catalog cat;
        std::vector<Column> ucols = {
            Column{ "users", "id", TypeKind::Int64, 1, 1 },
            Column{ "users", "name", TypeKind::Text, 2, 1 },
        };
        cat.register_table(std::make_shared<Table>("users", ucols));
        std::vector<Column> ocols = {
            Column{ "orders", "id", TypeKind::Int64, 1, 2 },
            Column{ "orders", "user_id", TypeKind::Int64, 2, 2 },
            Column{ "orders", "amount", TypeKind::Int64, 3, 2 },
        };
        cat.register_table(std::make_shared<Table>("orders", ocols));
        return cat;
    }

    LogicalPlanPtr plan_sql(Catalog& cat, const std::string& sql) {
        Parser parser;
        Statement stmt = parser.parse(sql);
        LogicalPlanner planner(cat);
        return planner.plan(stmt);
    }
} // namespace

TEST(LogicalPlanner, SelectStarProducesProjectScan) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT * FROM users");
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(plan->node);
    ASSERT_TRUE(proj.child);
    EXPECT_EQ(proj.child->kind, LogicalKind::Scan);
}

TEST(LogicalPlanner, SelectWithWhereProducesFilter) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT id FROM users WHERE id = 1");
    // Project → Filter → Scan
    ASSERT_EQ(plan->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(plan->node);
    ASSERT_EQ(proj.child->kind, LogicalKind::Filter);
    auto& filt = std::get<LogicalFilter>(proj.child->node);
    EXPECT_EQ(filt.child->kind, LogicalKind::Scan);
}

TEST(LogicalPlanner, JoinProducesLogicalJoin) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id");
    ASSERT_EQ(plan->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(plan->node);
    ASSERT_EQ(proj.child->kind, LogicalKind::Join);
    auto& j = std::get<LogicalJoin>(proj.child->node);
    EXPECT_EQ(j.left->kind, LogicalKind::Scan);
    EXPECT_EQ(j.right->kind, LogicalKind::Scan);
    EXPECT_TRUE(j.on.has_value());
}

TEST(LogicalPlanner, OrderByLimitProducesSortLimit) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT id FROM users ORDER BY id LIMIT 10");
    // Project → Limit → Sort → Scan
    ASSERT_EQ(plan->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(plan->node);
    ASSERT_EQ(proj.child->kind, LogicalKind::Limit);
    auto& lim = std::get<LogicalLimit>(proj.child->node);
    ASSERT_EQ(lim.child->kind, LogicalKind::Sort);
}

TEST(LogicalPlanner, InsertProducesDML) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "INSERT INTO users (id, name) VALUES (1, 'a')");
    ASSERT_EQ(plan->kind, LogicalKind::DML);
    auto& dml = std::get<LogicalDML>(plan->node);
    EXPECT_EQ(dml.kind, LogicalDML::Kind::Insert);
    ASSERT_TRUE(dml.source);
    EXPECT_EQ(dml.source->kind, LogicalKind::Values);
}

// ===== T4.4 R1: PredicatePushdown =====

TEST(R1PredicatePushdown, PushFilterThroughProject) {
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT id FROM users WHERE id = 1");
    // Initial shape: Project → Filter → Scan
    // After R1: should remain Project → Filter → Scan (filter is already at the
    // bottom-most useful position, but R1 may move filter under project if a
    // Project is above the filter — not the case here since planner builds
    // Project as the outermost node).
    auto applied = make_default_rules().apply(std::move(plan));
    ASSERT_TRUE(applied);
    // 当 Project 在 Filter 之上时，R1 会把 Filter 推到 Project 的 child 之下：
    // Project → Filter → Scan  此处 Filter 已经在 Project 之下，R1 应保持不变
    // 但若我们手动构造 Filter 在 Project 之上，则会下推。下面构造该场景：
    // 通过 raw 构造：Filter(Project(Scan))
    auto users = cat.lookup("users");
    auto scan = LogicalPlan::make_scan(users);
    LogicalColumn lc;
    ColumnRef cr;
    cr.table = "users";
    cr.name = "id";
    lc.expr = cr;
    lc.output_name = "id";
    auto proj = LogicalPlan::make_project({ lc }, std::move(scan));
    auto pred = cmp_eq(cr, Literal{ Value{ int64_t{ 1 } } });
    auto raw = LogicalPlan::make_filter(std::move(pred), std::move(proj));

    auto out = make_default_rules().apply(std::move(raw));
    // After R1, top should now be Project (Filter went below)
    EXPECT_EQ(out->kind, LogicalKind::Project);
    auto& p = std::get<LogicalProject>(out->node);
    EXPECT_EQ(p.child->kind, LogicalKind::Filter);
}

TEST(R1PredicatePushdown, PushIntoJoinChild) {
    auto cat = make_catalog_with_users_orders();
    // WHERE users.id = 1 引用单侧 → 应推到 join 的左侧
    auto plan = plan_sql(cat, "SELECT users.id FROM users JOIN orders ON users.id = orders.user_id "
                              "WHERE users.id = 1");
    auto out = make_default_rules().apply(std::move(plan));
    // 期望：Project → Join 且 Join 之上不再有 Filter，左侧 Scan 上方变成 Filter→Scan
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(out->node);
    ASSERT_EQ(proj.child->kind, LogicalKind::Join);
    auto& j = std::get<LogicalJoin>(proj.child->node);
    EXPECT_EQ(j.left->kind, LogicalKind::Filter) << "users.id = 1 应被下推到 join 左侧 users 之上";
    EXPECT_EQ(j.right->kind, LogicalKind::Scan) << "右侧 orders 不应有 filter";
}

TEST(R1PredicatePushdown, KeepFilterWhenBothSidesReferenced) {
    auto cat = make_catalog_with_users_orders();
    // WHERE users.id + orders.amount > 0 引用双侧 → Filter 必须留在 Join 之上
    // (这里 join 条件本身留在 ON，不影响)
    auto plan = plan_sql(cat, "SELECT users.id FROM users JOIN orders ON users.id = orders.user_id "
                              "WHERE users.id = orders.user_id");
    auto out = make_default_rules().apply(std::move(plan));
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(out->node);
    // 跨侧谓词应保留在 Join 之上
    EXPECT_EQ(proj.child->kind, LogicalKind::Filter) << "跨表谓词不应下推";
}

// ===== T4.5: PhysicalPlanner =====

TEST(PhysicalPlanner, SimpleScanProducesSeqScan) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT * FROM users");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    ASSERT_TRUE(pn);
    // Project → SeqScan
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    EXPECT_NE(dynamic_cast<SeqScanPlan*>(proj->child.get()), nullptr);
}

TEST(PhysicalPlanner, FilterProducesFilterPlan) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT id FROM users WHERE name = 'a'");
    auto applied = make_default_rules().apply(std::move(lp));
    PhysicalPlanner pp;
    auto pn = pp.plan(*applied);
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    auto* filt = dynamic_cast<FilterPlan*>(proj->child.get());
    ASSERT_NE(filt, nullptr) << "name 列无索引，应产生 FilterPlan";
    EXPECT_NE(dynamic_cast<SeqScanPlan*>(filt->child.get()), nullptr);
}

TEST(PhysicalPlanner, FilterOnIndexedColumnProducesIndexScan) {
    Catalog cat;
    std::vector<Column> cols = {
        Column{ "users", "id", TypeKind::Int64, 1, 1 },
        Column{ "users", "name", TypeKind::Text, 2, 1 },
    };
    auto t = std::make_shared<Table>("users", cols);
    t->create_index("id"); // 在 id 列上建索引
    cat.register_table(t);

    auto lp = plan_sql(cat, "SELECT name FROM users WHERE id = 42");
    auto applied = make_default_rules().apply(std::move(lp));
    PhysicalPlanner pp;
    auto pn = pp.plan(*applied);
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    // P1 选择：id 等值 + 有索引 → IndexScan，无 Filter 包裹
    EXPECT_NE(dynamic_cast<IndexScanPlan*>(proj->child.get()), nullptr) << "应触发 IndexScan 选择";
}

TEST(PhysicalPlanner, EquiJoinProducesHashJoin) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    EXPECT_NE(dynamic_cast<HashJoinPlan*>(proj->child.get()), nullptr) << "等值 join 应选 HashJoin";
}

TEST(PhysicalPlanner, OrderByLimitChain) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT id FROM users ORDER BY id LIMIT 10");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    // Project → Limit → OrderBy → SeqScan
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    auto* lim = dynamic_cast<LimitPlan*>(proj->child.get());
    ASSERT_NE(lim, nullptr);
    EXPECT_NE(dynamic_cast<OrderByPlan*>(lim->child.get()), nullptr);
}

TEST(PhysicalPlanner, InsertProducesInsertPlan) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "INSERT INTO users (id, name) VALUES (1, 'a'), (2, 'b')");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    auto* ins = dynamic_cast<InsertPlan*>(pn.get());
    ASSERT_NE(ins, nullptr);
    EXPECT_EQ(ins->rows.size(), 2u);
    EXPECT_EQ(ins->column_indexes.size(), 2u);
}

TEST(PhysicalPlanner, UpdateProducesUpdatePlan) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "UPDATE users SET name = 'x' WHERE id = 1");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    auto* upd = dynamic_cast<UpdatePlan*>(pn.get());
    ASSERT_NE(upd, nullptr);
    EXPECT_EQ(upd->assignments.size(), 1u);
    EXPECT_TRUE(upd->where.has_value());
}

TEST(PhysicalPlanner, DeleteProducesDeletePlan) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "DELETE FROM users WHERE id = 1");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    EXPECT_NE(dynamic_cast<DeletePlan*>(pn.get()), nullptr);
}

// ===== T4.7: R5 SmallTableLeftJoin =====
namespace {
    Catalog make_catalog_small_big() {
        // 构造两表：单列即可，仅用于 join 顺序验证
        Catalog cat;
        cat.register_table(std::make_shared<Table>(
                "small", std::vector<Column>{ Column{ "small", "id", TypeKind::Int64, 1, 1 } }));
        cat.register_table(
                std::make_shared<Table>("big", std::vector<Column>{ Column{ "big", "id", TypeKind::Int64, 1, 2 } }));
        return cat;
    }
} // namespace

TEST(R5JoinReorder, NoSwapWhenSidesEqualSize) {
    // 两个单 Scan，不应交换（保留用户书写顺序）
    auto cat = make_catalog_small_big();
    auto lp = plan_sql(cat, "SELECT small.id FROM small JOIN big ON small.id = big.id");
    auto out = make_default_rules().apply(std::move(lp));
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(out->node);
    ASSERT_EQ(proj.child->kind, LogicalKind::Join);
    auto& j = std::get<LogicalJoin>(proj.child->node);
    // 左右大小相等：不交换
    auto* lscan = std::get_if<LogicalScan>(&j.left->node);
    auto* rscan = std::get_if<LogicalScan>(&j.right->node);
    ASSERT_NE(lscan, nullptr);
    ASSERT_NE(rscan, nullptr);
    EXPECT_EQ(lscan->table->name(), "small");
    EXPECT_EQ(rscan->table->name(), "big");
}

TEST(R5JoinReorder, SwapsWhenLeftIsLargerNestedJoin) {
    // SELECT FROM (small JOIN big) AS lhs JOIN big2 ...
    // 通过嵌套 join 使左侧 size=2，右侧 size=1，应交换
    Catalog cat;
    cat.register_table(std::make_shared<Table>("a", std::vector<Column>{ Column{ "a", "id", TypeKind::Int64, 1, 1 } }));
    cat.register_table(std::make_shared<Table>("b", std::vector<Column>{ Column{ "b", "id", TypeKind::Int64, 1, 2 } }));
    cat.register_table(std::make_shared<Table>("c", std::vector<Column>{ Column{ "c", "id", TypeKind::Int64, 1, 3 } }));
    // 解析器不一定支持括号嵌套——退而求其次，直接构造 LogicalPlan
    auto a_scan = LogicalPlan::make_scan(cat.lookup("a"));
    auto b_scan = LogicalPlan::make_scan(cat.lookup("b"));
    auto c_scan = LogicalPlan::make_scan(cat.lookup("c"));
    BoolExpr ab_on;
    ab_on.kind = BoolExpr::Kind::Comparison;
    Comparison cab;
    cab.op = CompareOp::Eq;
    cab.lhs = ColumnRef{ "a", "id", 0, 0 };
    cab.rhs = ColumnRef{ "b", "id", 0, 0 };
    ab_on.cmp = std::move(cab);
    auto ab_join = LogicalPlan::make(
            LogicalKind::Join, LogicalJoin{ JoinType::Inner, std::move(ab_on), std::move(a_scan), std::move(b_scan) });
    BoolExpr abc_on;
    abc_on.kind = BoolExpr::Kind::Comparison;
    Comparison cabc;
    cabc.op = CompareOp::Eq;
    cabc.lhs = ColumnRef{ "a", "id", 0, 0 };
    cabc.rhs = ColumnRef{ "c", "id", 0, 0 };
    abc_on.cmp = std::move(cabc);
    // 故意把 (a JOIN b) 放左、c 放右 → R5 应交换为 c 在左
    auto root = LogicalPlan::make(LogicalKind::Join, LogicalJoin{ JoinType::Inner, std::move(abc_on),
                                                                  std::move(ab_join), std::move(c_scan) });
    auto out = make_default_rules().apply(std::move(root));
    ASSERT_EQ(out->kind, LogicalKind::Join);
    auto& j = std::get<LogicalJoin>(out->node);
    // 交换后：左应为单 Scan c
    EXPECT_EQ(j.left->kind, LogicalKind::Scan) << "R5 应把更小的 c 子树移到左侧";
    EXPECT_EQ(j.right->kind, LogicalKind::Join) << "右侧应是较大的 (a JOIN b) 子树";
}

// ===== T4.7: R4 ColumnPruning =====
TEST(R4ColumnPruning, PrunesInnerProjectColumns) {
    // 顶层 Project(id) → Project(id, name) → Scan(users)
    auto cat = make_catalog_with_users_orders();
    auto scan = LogicalPlan::make_scan(cat.lookup("users"));
    LogicalColumn ic1;
    ic1.expr = ColumnRef{ "users", "id", 0, 0 };
    ic1.output_name = "id";
    LogicalColumn ic2;
    ic2.expr = ColumnRef{ "users", "name", 0, 0 };
    ic2.output_name = "name";
    std::vector<LogicalColumn> inner_cols;
    inner_cols.push_back(std::move(ic1));
    inner_cols.push_back(std::move(ic2));
    auto inner = LogicalPlan::make_project(std::move(inner_cols), std::move(scan));
    LogicalColumn oc1;
    oc1.expr = ColumnRef{ "users", "id", 0, 0 };
    oc1.output_name = "id";
    std::vector<LogicalColumn> outer_cols;
    outer_cols.push_back(std::move(oc1));
    auto outer = LogicalPlan::make_project(std::move(outer_cols), std::move(inner));

    auto out = make_default_rules().apply(std::move(outer));
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& outer_p = std::get<LogicalProject>(out->node);
    ASSERT_EQ(outer_p.child->kind, LogicalKind::Project);
    auto& inner_p = std::get<LogicalProject>(outer_p.child->node);
    EXPECT_EQ(inner_p.columns.size(), 1u) << "name 列未被外层引用，应被裁剪";
    EXPECT_EQ(inner_p.columns[0].output_name, "id");
}

TEST(R4ColumnPruning, KeepsAllColumnsAtRoot) {
    // 顶层 Project 不应被裁剪（用户语义）
    auto cat = make_catalog_with_users_orders();
    auto plan = plan_sql(cat, "SELECT id, name FROM users");
    auto out = make_default_rules().apply(std::move(plan));
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& proj = std::get<LogicalProject>(out->node);
    EXPECT_EQ(proj.columns.size(), 2u);
}

// ===== T4.4: R2 ProjectionMerge =====
TEST(R2ProjectionMerge, MergesAdjacentProjects) {
    // Project(id) → Project(id, name) → Scan(users)
    // R4 先裁剪内层为 Project(id) → R2 合并相邻 Project → Project(id) → Scan
    auto cat = make_catalog_with_users_orders();
    auto scan = LogicalPlan::make_scan(cat.lookup("users"));
    LogicalColumn ic1;
    ic1.expr = ColumnRef{ "users", "id", 0, 0 };
    ic1.output_name = "id";
    LogicalColumn ic2;
    ic2.expr = ColumnRef{ "users", "name", 0, 0 };
    ic2.output_name = "name";
    std::vector<LogicalColumn> inner_cols;
    inner_cols.push_back(std::move(ic1));
    inner_cols.push_back(std::move(ic2));
    auto inner = LogicalPlan::make_project(std::move(inner_cols), std::move(scan));
    LogicalColumn oc;
    oc.expr = ColumnRef{ "", "id", 0, 0 };
    oc.output_name = "id";
    std::vector<LogicalColumn> outer_cols;
    outer_cols.push_back(std::move(oc));
    auto outer = LogicalPlan::make_project(std::move(outer_cols), std::move(inner));

    auto out = make_default_rules().apply(std::move(outer));
    ASSERT_EQ(out->kind, LogicalKind::Project);
    auto& p = std::get<LogicalProject>(out->node);
    // 合并后：直接 Project → Scan，不再有内层 Project
    ASSERT_EQ(p.child->kind, LogicalKind::Scan) << "R2 应消除冗余的相邻 Project 层";
    EXPECT_EQ(p.columns.size(), 1u);
    EXPECT_EQ(p.columns[0].output_name, "id");
}

// ===== T4.6: P3 MergeJoin (sorted children) =====
TEST(PhysicalPlanner, SortedChildrenProduceMergeJoin) {
    auto cat = make_catalog_with_users_orders();
    auto users_tbl = cat.lookup("users");
    auto orders_tbl = cat.lookup("orders");

    auto lscan = LogicalPlan::make_scan(users_tbl);
    auto rscan = LogicalPlan::make_scan(orders_tbl);

    LogicalSortKey lk;
    lk.expr = ColumnRef{ "users", "id", 0, 0 };
    lk.ascending = true;
    LogicalSortKey rk;
    rk.expr = ColumnRef{ "orders", "user_id", 0, 0 };
    rk.ascending = true;
    LogicalSort lsort;
    lsort.keys.push_back(std::move(lk));
    lsort.child = std::move(lscan);
    LogicalSort rsort;
    rsort.keys.push_back(std::move(rk));
    rsort.child = std::move(rscan);
    auto lsorted = LogicalPlan::make(LogicalKind::Sort, std::move(lsort));
    auto rsorted = LogicalPlan::make(LogicalKind::Sort, std::move(rsort));

    Comparison cmp;
    cmp.op = CompareOp::Eq;
    cmp.lhs = ColumnRef{ "users", "id", 0, 0 };
    cmp.rhs = ColumnRef{ "orders", "user_id", 0, 0 };
    BoolExpr on_pred = BoolExpr::make_comparison(std::move(cmp));

    LogicalJoin lj;
    lj.join_type = JoinType::Inner;
    lj.on = on_pred;
    lj.left = std::move(lsorted);
    lj.right = std::move(rsorted);
    auto join = LogicalPlan::make(LogicalKind::Join, std::move(lj));

    PhysicalPlanner pp;
    auto pn = pp.plan(*join);
    EXPECT_NE(dynamic_cast<MergeJoinPlan*>(pn.get()), nullptr) << "已按等值 join 键排序的双侧应选 MergeJoin";
}

TEST(PhysicalPlanner, UnsortedJoinFallsBackToHashJoin) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id");
    PhysicalPlanner pp;
    auto pn = pp.plan(*lp);
    auto* proj = dynamic_cast<ProjectPlan*>(pn.get());
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ(dynamic_cast<MergeJoinPlan*>(proj->child.get()), nullptr);
    EXPECT_NE(dynamic_cast<HashJoinPlan*>(proj->child.get()), nullptr);
}

// ===== T7.3: 单规则 before/after 测试 =====
// 通过 RuleSet 仅注册某条规则，验证该规则在隔离环境下的行为。

namespace {
    LogicalPlanPtr apply_single(RulePtr rule, LogicalPlanPtr p) {
        RuleSet rs;
        rs.add(std::move(rule));
        return rs.apply(std::move(p));
    }
} // namespace

TEST(SingleRule_PredicatePushdown, PushFilterIntoJoin) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT users.id FROM users JOIN orders ON users.id = orders.user_id WHERE users.id = 5");
    // before: 顶层应有一个 Project, 子节点链中 Filter 在 Join 之上
    auto before_str = to_string(*lp);
    EXPECT_NE(before_str.find("Filter"), std::string::npos);

    auto after = apply_single(make_predicate_pushdown_rule(), std::move(lp));
    auto after_str = to_string(*after);
    // after: 谓词应该被推到 users 表 Scan 之上（Filter 出现在 Scan 紧邻位置）
    // 简易判定：Filter 仍存在但 plan 结构发生变化。
    EXPECT_NE(after_str.find("Filter"), std::string::npos);
    EXPECT_NE(after_str, before_str) << "PredicatePushdown 应改变 plan 结构";
}

TEST(SingleRule_ColumnPruning, PrunesInnerColumns) {
    auto cat = make_catalog_with_users_orders();
    auto scan = LogicalPlan::make_scan(cat.lookup("users"));
    LogicalColumn ic1;
    ic1.expr = ColumnRef{ "users", "id", 0, 0 };
    ic1.output_name = "id";
    LogicalColumn ic2;
    ic2.expr = ColumnRef{ "users", "name", 0, 0 };
    ic2.output_name = "name";
    std::vector<LogicalColumn> inner_cols;
    inner_cols.push_back(std::move(ic1));
    inner_cols.push_back(std::move(ic2));
    auto inner = LogicalPlan::make_project(std::move(inner_cols), std::move(scan));
    LogicalColumn oc;
    oc.expr = ColumnRef{ "", "id", 0, 0 };
    oc.output_name = "id";
    std::vector<LogicalColumn> outer_cols;
    outer_cols.push_back(std::move(oc));
    auto outer = LogicalPlan::make_project(std::move(outer_cols), std::move(inner));

    auto after = apply_single(make_column_pruning_rule(), std::move(outer));
    ASSERT_EQ(after->kind, LogicalKind::Project);
    auto& outerp = std::get<LogicalProject>(after->node);
    ASSERT_EQ(outerp.child->kind, LogicalKind::Project);
    auto& innerp = std::get<LogicalProject>(outerp.child->node);
    EXPECT_EQ(innerp.columns.size(), 1u) << "内层只保留外层用到的 id 列";
    EXPECT_EQ(innerp.columns[0].output_name, "id");
}

TEST(SingleRule_ProjectionMerge, MergesAdjacentProjects) {
    auto cat = make_catalog_with_users_orders();
    auto scan = LogicalPlan::make_scan(cat.lookup("users"));
    LogicalColumn ic;
    ic.expr = ColumnRef{ "users", "id", 0, 0 };
    ic.output_name = "id";
    std::vector<LogicalColumn> inner_cols;
    inner_cols.push_back(std::move(ic));
    auto inner = LogicalPlan::make_project(std::move(inner_cols), std::move(scan));
    LogicalColumn oc;
    oc.expr = ColumnRef{ "", "id", 0, 0 };
    oc.output_name = "id";
    std::vector<LogicalColumn> outer_cols;
    outer_cols.push_back(std::move(oc));
    auto outer = LogicalPlan::make_project(std::move(outer_cols), std::move(inner));

    auto after = apply_single(make_projection_merge_rule(), std::move(outer));
    ASSERT_EQ(after->kind, LogicalKind::Project);
    auto& p = std::get<LogicalProject>(after->node);
    EXPECT_EQ(p.child->kind, LogicalKind::Scan) << "ProjectionMerge 应消除内层 Project";
}

TEST(SingleRule_JoinReorder, SwapsLargerLeft) {
    auto cat = make_catalog_with_users_orders();
    // 构造 Join(Join(Scan, Scan), Scan) — 左侧子树 size=2, 右侧 size=1
    auto s1 = LogicalPlan::make_scan(cat.lookup("users"));
    auto s2 = LogicalPlan::make_scan(cat.lookup("orders"));
    auto s3 = LogicalPlan::make_scan(cat.lookup("users"));

    Comparison cmp1;
    cmp1.op = CompareOp::Eq;
    cmp1.lhs = ColumnRef{ "users", "id", 0, 0 };
    cmp1.rhs = ColumnRef{ "orders", "user_id", 0, 0 };
    LogicalJoin inner_j;
    inner_j.join_type = JoinType::Inner;
    inner_j.on = BoolExpr::make_comparison(std::move(cmp1));
    inner_j.left = std::move(s1);
    inner_j.right = std::move(s2);
    auto inner_join = LogicalPlan::make(LogicalKind::Join, std::move(inner_j));

    Comparison cmp2;
    cmp2.op = CompareOp::Eq;
    cmp2.lhs = ColumnRef{ "users", "id", 0, 0 };
    cmp2.rhs = ColumnRef{ "users", "id", 0, 0 };
    LogicalJoin outer_j;
    outer_j.join_type = JoinType::Inner;
    outer_j.on = BoolExpr::make_comparison(std::move(cmp2));
    outer_j.left = std::move(inner_join);
    outer_j.right = std::move(s3);
    auto root = LogicalPlan::make(LogicalKind::Join, std::move(outer_j));

    auto after = apply_single(make_join_reorder_rule(), std::move(root));
    ASSERT_EQ(after->kind, LogicalKind::Join);
    auto& j = std::get<LogicalJoin>(after->node);
    // 交换后左侧应为 Scan（小），右侧为 Join（大）
    EXPECT_EQ(j.left->kind, LogicalKind::Scan);
    EXPECT_EQ(j.right->kind, LogicalKind::Join);
}

TEST(SingleRule_ConstantFolding, FoldsArithmeticInPredicate) {
    // WHERE id = 1+1 → 1+1 折叠为 2；ApplyStats 应记录改写。
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT id FROM users WHERE id = 1 + 1");
    RuleSet rs;
    rs.add(make_constant_folding_rule());
    RuleSet::ApplyStats stats;
    auto after = rs.apply(std::move(lp), &stats);
    ASSERT_TRUE(after);
    EXPECT_GT(stats.total_rewrites, 0) << "ConstantFolding 应将 1+1 折叠为 2";
}


// =====================================================================
// T9.6.5: HashAggregate vs SortAggregate 策略选择
// =====================================================================

TEST(PhysicalPlannerAggregate, ChoosesHashWhenChildNotSorted) {
    auto cat = make_catalog_with_users_orders();
    auto lp = plan_sql(cat, "SELECT user_id, COUNT(*) FROM orders GROUP BY user_id");
    PhysicalPlanner pp(cat, nullptr);
    auto phys = pp.plan(*lp);
    ASSERT_TRUE(phys);
    auto* agg = dynamic_cast<const AggregatePlan*>(phys.get());
    ASSERT_TRUE(agg) << "should produce AggregatePlan";
    EXPECT_EQ(agg->strategy, AggregatePlan::Strategy::Hash);
}

TEST(PhysicalPlannerAggregate, ChoosesSortWhenChildSortedByGroupKey) {
    // SQL 不会自然产生 Sort 在 Aggregate 之下；手工组装 Aggregate(Sort(Scan)) 验证策略。
    auto cat = make_catalog_with_users_orders();

    // Scan(orders)
    auto scan = LogicalPlan::make_scan(cat.lookup("orders"));

    // Sort(by user_id)
    LogicalSort ls;
    LogicalSortKey k;
    k.expr = Expression{ ColumnRef{ "orders", "user_id" } };
    k.ascending = true;
    ls.keys.push_back(k);
    ls.child = std::move(scan);
    auto sort = LogicalPlan::make(LogicalKind::Sort, std::move(ls));

    // Aggregate(group_by=user_id, agg=count)
    LogicalAggregate la;
    la.group_by.push_back(Expression{ ColumnRef{ "orders", "user_id" } });
    LogicalAggregateItem ai;
    ai.func = AggFunc::Count;
    ai.output_name = "cnt";
    la.aggregates.push_back(ai);
    la.child = std::move(sort);
    auto agg = LogicalPlan::make(LogicalKind::Aggregate, std::move(la));

    PhysicalPlanner pp(cat, nullptr);
    auto phys = pp.plan(*agg);
    ASSERT_TRUE(phys);
    auto* ap = dynamic_cast<const AggregatePlan*>(phys.get());
    ASSERT_TRUE(ap) << "should produce AggregatePlan";
    EXPECT_EQ(ap->strategy, AggregatePlan::Strategy::Sort)
            << "should pick SortAggregate when child sorted by group key";
}
