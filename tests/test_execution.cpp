/**
 * @file test_execution.cpp
 * @brief 执行器和计划器单元测试
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 测试查询执行器和查询计划器
 */

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "corodb/ast/ast.h"
#include "corodb/common/types.h"
#include "corodb/executor/executor.h"
#include "corodb/executor/expression_evaluator.h"
#include "corodb/executor/bool_evaluator.h"
#include "corodb/optimizer/logical/logical_planner.h"
#include "corodb/optimizer/physical/physical_planner.h"
#include "corodb/optimizer/logical/rule.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/storage/lsm_storage_engine.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/storage/table.h"

using namespace corodb;

// ============================================================================
// Binding 测试
// ============================================================================

class BindingTest : public ::testing::Test {};

TEST_F(BindingTest, QualifiedName) {
    Binding binding{ "users", "name", 0, 1, 0 };
    EXPECT_EQ(binding.qualified_name(), "users.name");
}

TEST_F(BindingTest, QualifiedNameWithoutTable) {
    Binding binding{ "", "column1", 0, 0, 0 };
    EXPECT_EQ(binding.qualified_name(), "column1");
}

TEST_F(BindingTest, MatchesColumnRef) {
    Binding binding{ "users", "name", 0, 1, 0 };

    ColumnRef ref1{ "users", "name" };
    ColumnRef ref2{ "", "name" };
    ColumnRef ref3{ "orders", "name" };
    ColumnRef ref4{ "users", "id" };

    EXPECT_TRUE(binding.matches(ref1));
    EXPECT_TRUE(binding.matches(ref2));  // 无表名限定可以匹配
    EXPECT_FALSE(binding.matches(ref3)); // 不同表名不匹配
    EXPECT_FALSE(binding.matches(ref4)); // 不同列名不匹配
}

// ============================================================================
// Record 测试
// ============================================================================

class RecordTest : public ::testing::Test {};

TEST_F(RecordTest, EmptyRecord) {
    Record record;
    EXPECT_TRUE(record.empty());
    EXPECT_EQ(record.size(), 0u);
}

TEST_F(RecordTest, RecordWithValues) {
    Record record;
    record.values = { int64_t{ 1 }, std::string{ "Alice" }, int64_t{ 25 } };
    record.bindings = { { "users", "id", 0, 1, 0 }, { "users", "name", 1, 1, 0 }, { "users", "age", 2, 1, 0 } };

    EXPECT_EQ(record.size(), 3u);
    EXPECT_FALSE(record.empty());
}

// ============================================================================
// lookup_value 测试
// ============================================================================

class LookupValueTest : public ::testing::Test {
protected:
    Record record;

    void SetUp() override {
        record.values = { int64_t{ 42 }, std::string{ "TestName" }, NullValue{} };
        record.bindings = { { "users", "id", 0, 1, 0 }, { "users", "name", 1, 1, 0 }, { "users", "extra", 2, 1, 0 } };
    }
};

TEST_F(LookupValueTest, LookupByColumnName) {
    ColumnRef ref{ "", "name" };
    const Value& val = ExpressionEvaluator::lookup(record, ref);

    ASSERT_TRUE(std::holds_alternative<std::string>(val));
    EXPECT_EQ(std::get<std::string>(val), "TestName");
}

TEST_F(LookupValueTest, LookupByQualifiedName) {
    ColumnRef ref{ "users", "id" };
    const Value& val = ExpressionEvaluator::lookup(record, ref);

    ASSERT_TRUE(std::holds_alternative<int64_t>(val));
    EXPECT_EQ(std::get<int64_t>(val), 42);
}

TEST_F(LookupValueTest, LookupNullValue) {
    ColumnRef ref{ "users", "extra" };
    const Value& val = ExpressionEvaluator::lookup(record, ref);

    EXPECT_TRUE(std::holds_alternative<NullValue>(val));
}

TEST_F(LookupValueTest, LookupNonexistentColumn) {
    ColumnRef ref{ "users", "nonexistent" };
    EXPECT_THROW(ExpressionEvaluator::lookup(record, ref), std::runtime_error);
}

// ============================================================================
// eval_expression 测试
// ============================================================================

class EvalExpressionTest : public ::testing::Test {
protected:
    Record record;

    void SetUp() override {
        record.values = { int64_t{ 10 }, int64_t{ 20 }, std::string{ "test" } };
        record.bindings = { { "t", "a", 0, 1, 0 }, { "t", "b", 1, 1, 0 }, { "t", "s", 2, 1, 0 } };
    }
};

TEST_F(EvalExpressionTest, EvalColumnRef) {
    ColumnRef ref{ "t", "a" };
    Expression expr = ref;

    Value result = ExpressionEvaluator::eval(record, expr);
    ASSERT_TRUE(std::holds_alternative<int64_t>(result));
    EXPECT_EQ(std::get<int64_t>(result), 10);
}

TEST_F(EvalExpressionTest, EvalIntLiteral) {
    Literal lit = Literal::from_int(42);
    Expression expr = lit;

    Value result = ExpressionEvaluator::eval(record, expr);
    ASSERT_TRUE(std::holds_alternative<int64_t>(result));
    EXPECT_EQ(std::get<int64_t>(result), 42);
}

TEST_F(EvalExpressionTest, EvalStringLiteral) {
    Literal lit = Literal::from_string("hello");
    Expression expr = lit;

    Value result = ExpressionEvaluator::eval(record, expr);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), "hello");
}

TEST_F(EvalExpressionTest, EvalNullLiteral) {
    Literal lit = Literal::null();
    Expression expr = lit;

    Value result = ExpressionEvaluator::eval(record, expr);
    EXPECT_TRUE(std::holds_alternative<NullValue>(result));
}

// ============================================================================
// compare_order 测试
// ============================================================================

class CompareOrderTest : public ::testing::Test {};

TEST_F(CompareOrderTest, CompareIntegers) {
    Value v1 = int64_t{ 10 };
    Value v2 = int64_t{ 20 };
    Value v3 = int64_t{ 10 };

    EXPECT_LT(BoolEvaluator::compare_order(v1, v2), 0);
    EXPECT_GT(BoolEvaluator::compare_order(v2, v1), 0);
    EXPECT_EQ(BoolEvaluator::compare_order(v1, v3), 0);
}

TEST_F(CompareOrderTest, CompareStrings) {
    Value v1 = std::string{ "apple" };
    Value v2 = std::string{ "banana" };
    Value v3 = std::string{ "apple" };

    EXPECT_LT(BoolEvaluator::compare_order(v1, v2), 0);
    EXPECT_GT(BoolEvaluator::compare_order(v2, v1), 0);
    EXPECT_EQ(BoolEvaluator::compare_order(v1, v3), 0);
}

TEST_F(CompareOrderTest, CompareNullWithNull) {
    Value v1 = NullValue{};
    Value v2 = NullValue{};

    EXPECT_EQ(BoolEvaluator::compare_order(v1, v2), 0);
}

TEST_F(CompareOrderTest, NullsLast) {
    Value null_val = NullValue{};
    Value int_val = int64_t{ 0 };

    // NULL 排在最后
    EXPECT_GT(BoolEvaluator::compare_order(null_val, int_val), 0);
    EXPECT_LT(BoolEvaluator::compare_order(int_val, null_val), 0);
}

TEST_F(CompareOrderTest, CompareNegativeNumbers) {
    Value v1 = int64_t{ -10 };
    Value v2 = int64_t{ 10 };
    Value v3 = int64_t{ -20 };

    EXPECT_LT(BoolEvaluator::compare_order(v1, v2), 0);
    EXPECT_GT(BoolEvaluator::compare_order(v1, v3), 0);
}

// ============================================================================
// evaluate_comparison 测试
// ============================================================================

class EvaluateComparisonTest : public ::testing::Test {
protected:
    Record record;

    void SetUp() override {
        record.values = { int64_t{ 42 }, std::string{ "Alice" } };
        record.bindings = { { "users", "id", 0, 1, 0 }, { "users", "name", 1, 1, 0 } };
    }
};

TEST_F(EvaluateComparisonTest, EqualComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Eq;
    cmp.rhs = Literal::from_int(42);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);

    cmp.rhs = Literal::from_int(100);
    bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::False);
}

TEST_F(EvaluateComparisonTest, NotEqualComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Ne;
    cmp.rhs = Literal::from_int(100);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);
}

TEST_F(EvaluateComparisonTest, LessThanComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Lt;
    cmp.rhs = Literal::from_int(100);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);

    cmp.rhs = Literal::from_int(10);
    bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::False);
}

TEST_F(EvaluateComparisonTest, LessOrEqualComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Le;
    cmp.rhs = Literal::from_int(42);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);
}

TEST_F(EvaluateComparisonTest, GreaterThanComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Gt;
    cmp.rhs = Literal::from_int(10);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);
}

TEST_F(EvaluateComparisonTest, GreaterOrEqualComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "id" };
    cmp.op = CompareOp::Ge;
    cmp.rhs = Literal::from_int(42);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);
}

TEST_F(EvaluateComparisonTest, StringComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "name" };
    cmp.op = CompareOp::Eq;
    cmp.rhs = Literal::from_string("Alice");

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, record), SqlBool::True);
}

TEST_F(EvaluateComparisonTest, ComparisonWithNull) {
    // 涉及 NULL 的比较返回 false
    Record rec_with_null;
    rec_with_null.values = { NullValue{} };
    rec_with_null.bindings = { { "t", "col", 0, 1, 0 } };

    Comparison cmp;
    cmp.lhs = ColumnRef{ "t", "col" };
    cmp.op = CompareOp::Eq;
    cmp.rhs = Literal::from_int(0);

    BoolExpr bool_expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(bool_expr, rec_with_null), SqlBool::Unknown);
}

// ============================================================================
// evaluate_bool_expr 测试
// ============================================================================

class EvaluateBoolExprTest : public ::testing::Test {
protected:
    Record record;

    void SetUp() override {
        record.values = { int64_t{ 25 }, std::string{ "Bob" } };
        record.bindings = { { "users", "age", 0, 1, 0 }, { "users", "name", 1, 1, 0 } };
    }
};

TEST_F(EvaluateBoolExprTest, SimpleComparison) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "age" };
    cmp.op = CompareOp::Gt;
    cmp.rhs = Literal::from_int(20);

    BoolExpr expr = BoolExpr::make_comparison(cmp);
    EXPECT_EQ(BoolEvaluator::eval(expr, record), SqlBool::True);
}

TEST_F(EvaluateBoolExprTest, AndExpression) {
    Comparison cmp1;
    cmp1.lhs = ColumnRef{ "users", "age" };
    cmp1.op = CompareOp::Gt;
    cmp1.rhs = Literal::from_int(20);

    Comparison cmp2;
    cmp2.lhs = ColumnRef{ "users", "age" };
    cmp2.op = CompareOp::Lt;
    cmp2.rhs = Literal::from_int(30);

    BoolExpr expr = BoolExpr::make_and(BoolExpr::make_comparison(cmp1), BoolExpr::make_comparison(cmp2));
    EXPECT_EQ(BoolEvaluator::eval(expr, record), SqlBool::True);
}

TEST_F(EvaluateBoolExprTest, OrExpression) {
    Comparison cmp1;
    cmp1.lhs = ColumnRef{ "users", "age" };
    cmp1.op = CompareOp::Lt;
    cmp1.rhs = Literal::from_int(20);

    Comparison cmp2;
    cmp2.lhs = ColumnRef{ "users", "age" };
    cmp2.op = CompareOp::Gt;
    cmp2.rhs = Literal::from_int(24);

    BoolExpr expr = BoolExpr::make_or(BoolExpr::make_comparison(cmp1), BoolExpr::make_comparison(cmp2));
    EXPECT_EQ(BoolEvaluator::eval(expr, record), SqlBool::True);
}

TEST_F(EvaluateBoolExprTest, NotExpression) {
    Comparison cmp;
    cmp.lhs = ColumnRef{ "users", "age" };
    cmp.op = CompareOp::Lt;
    cmp.rhs = Literal::from_int(20);

    BoolExpr expr = BoolExpr::make_not(BoolExpr::make_comparison(cmp));
    EXPECT_EQ(BoolEvaluator::eval(expr, record), SqlBool::True);
}

// ============================================================================
// PhysicalPlanner 测试（T9.4.1：legacy Planner 已删除，统一走 LogicalPlanner + Rules + PhysicalPlanner）
// ============================================================================

class PlannerTest : public ::testing::Test {
protected:
    std::unique_ptr<StorageEngine> engine;
    Catalog catalog;
    std::filesystem::path temp_dir;

    std::unique_ptr<PlanNode> plan_stmt(const Statement& stmt) {
        opt::LogicalPlanner lp{ catalog };
        auto logical = lp.plan(stmt);
        opt::RuleSet rules = opt::make_default_rules();
        logical = rules.apply(std::move(logical));
        opt::PhysicalPlanner pp{ catalog, engine.get() };
        return pp.plan(*logical);
    }

    void SetUp() override {
        temp_dir = std::filesystem::temp_directory_path() / "corodb_planner_test";
        std::filesystem::create_directories(temp_dir);

        engine = std::make_unique<LSMTreeEngine>(temp_dir.string());

        std::vector<Column> user_cols = { Column{ "users", "id", TypeKind::Int64, 1, 1 },
                                          Column{ "users", "name", TypeKind::Text, 2, 1 } };
        auto users_table = std::make_shared<Table>("users", user_cols, engine.get());
        catalog.register_table(users_table);
    }

    void TearDown() override {
        engine.reset();
        std::filesystem::remove_all(temp_dir);
    }
};

TEST_F(PlannerTest, PlanSimpleSelect) {
    SelectStmt select;
    select.from_table = "users";

    Statement stmt = select;
    auto plan = plan_stmt(stmt);

    ASSERT_NE(plan, nullptr);
}

TEST_F(PlannerTest, PlanInsert) {
    InsertStmt insert;
    insert.table = "users";
    insert.rows = { { Literal::from_int(1), Literal::from_string("Alice") } };

    Statement stmt = insert;
    auto plan = plan_stmt(stmt);

    ASSERT_NE(plan, nullptr);
}

TEST_F(PlannerTest, PlanInvalidTable) {
    SelectStmt select;
    select.from_table = "nonexistent_table";

    Statement stmt = select;
    EXPECT_THROW(plan_stmt(stmt), std::exception);
}

// ============================================================================
// CompareOp 测试
// ============================================================================

TEST(CompareOpTest, AllOperators) {
    // 确保所有比较运算符都定义
    CompareOp ops[] = { CompareOp::Eq, CompareOp::Ne, CompareOp::Lt, CompareOp::Le, CompareOp::Gt, CompareOp::Ge };

    EXPECT_EQ(sizeof(ops) / sizeof(ops[0]), 6u);
}

TEST(CompareOpTest, CompareOpToString) {
    EXPECT_STREQ(compare_op_str(CompareOp::Eq), "=");
    EXPECT_STREQ(compare_op_str(CompareOp::Ne), "<>");
    EXPECT_STREQ(compare_op_str(CompareOp::Lt), "<");
    EXPECT_STREQ(compare_op_str(CompareOp::Le), "<=");
    EXPECT_STREQ(compare_op_str(CompareOp::Gt), ">");
    EXPECT_STREQ(compare_op_str(CompareOp::Ge), ">=");
}

