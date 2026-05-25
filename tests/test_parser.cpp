/**
 * @file test_parser.cpp
 * @brief SQL 解析器单元测试
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 测试 SQL 解析器的各种功能
 */

#include <gtest/gtest.h>
#include <variant>

#include "corodb/ast/ast.h"
#include "corodb/sql/parser.h"

using namespace corodb;

// ============================================================================
// 辅助函数测试
// ============================================================================

TEST(ToUpperTest, LowerCaseToUpper) {
    EXPECT_EQ(to_upper("select"), "SELECT");
    EXPECT_EQ(to_upper("from"), "FROM");
    EXPECT_EQ(to_upper("where"), "WHERE");
}

TEST(ToUpperTest, MixedCase) {
    EXPECT_EQ(to_upper("SeLeCt"), "SELECT");
    EXPECT_EQ(to_upper("fRoM"), "FROM");
}

TEST(ToUpperTest, AlreadyUpperCase) {
    EXPECT_EQ(to_upper("SELECT"), "SELECT");
    EXPECT_EQ(to_upper("CREATE"), "CREATE");
}

TEST(ToUpperTest, EmptyString) {
    EXPECT_EQ(to_upper(""), "");
}

TEST(ToUpperTest, NonAlphabetic) {
    EXPECT_EQ(to_upper("123"), "123");
    EXPECT_EQ(to_upper("_test_"), "_TEST_");
}

// ============================================================================
// Parser 基础测试
// ============================================================================

class ParserTest : public ::testing::Test {
protected:
    Parser parser;
};

// ============================================================================
// SELECT 语句测试
// ============================================================================

TEST_F(ParserTest, SimpleSelect) {
    auto stmt = parser.parse("SELECT * FROM users");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_EQ(select.from_table, "users");
    // Note: SELECT * may have projections (e.g., a wildcard marker)
}

TEST_F(ParserTest, SelectWithColumns) {
    auto stmt = parser.parse("SELECT id, name FROM users");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_EQ(select.projections.size(), 2u);
}

TEST_F(ParserTest, SelectWithWhere) {
    auto stmt = parser.parse("SELECT * FROM users WHERE id = 1");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_TRUE(select.where.has_value());
}

TEST_F(ParserTest, SelectWithOrderBy) {
    auto stmt = parser.parse("SELECT * FROM users ORDER BY name");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_FALSE(select.order_by.empty());
}

TEST_F(ParserTest, SelectWithOrderByDesc) {
    auto stmt = parser.parse("SELECT * FROM users ORDER BY name DESC");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_FALSE(select.order_by.empty());
    EXPECT_FALSE(select.order_by[0].asc);
}

TEST_F(ParserTest, SelectWithGroupBy) {
    auto stmt = parser.parse("SELECT department, COUNT(*) FROM employees GROUP BY department");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_FALSE(select.group_by.empty());
}

TEST_F(ParserTest, SelectWithLimit) {
    auto stmt = parser.parse("SELECT * FROM users LIMIT 10");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_TRUE(select.limit.has_value());
    EXPECT_EQ(select.limit.value(), 10);
}

TEST_F(ParserTest, SelectWithJoin) {
    auto stmt = parser.parse("SELECT * FROM users JOIN orders ON users.id = orders.user_id");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

TEST_F(ParserTest, SelectWithLeftJoin) {
    auto stmt = parser.parse("SELECT * FROM users LEFT JOIN orders ON users.id = orders.user_id");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

// ============================================================================
// INSERT 语句测试
// ============================================================================

TEST_F(ParserTest, SimpleInsert) {
    auto stmt = parser.parse("INSERT INTO users VALUES (1, 'Alice')");
    ASSERT_TRUE(std::holds_alternative<InsertStmt>(stmt));

    const auto& insert = std::get<InsertStmt>(stmt);
    EXPECT_EQ(insert.table, "users");
    ASSERT_EQ(insert.rows.size(), 1u);
    EXPECT_EQ(insert.rows[0].size(), 2u);
}

// 注意: 当前解析器不支持 INSERT VALUES 中的 NULL 字面量
// TEST_F(ParserTest, InsertWithNullValue) { ... }

TEST_F(ParserTest, InsertWithColumnList) {
    auto stmt = parser.parse("INSERT INTO users (id, name) VALUES (1, 'Alice')");
    ASSERT_TRUE(std::holds_alternative<InsertStmt>(stmt));
}

// ============================================================================
// UPDATE 语句测试
// ============================================================================

TEST_F(ParserTest, SimpleUpdate) {
    auto stmt = parser.parse("UPDATE users SET name = 'Bob' WHERE id = 1");
    ASSERT_TRUE(std::holds_alternative<UpdateStmt>(stmt));

    const auto& update = std::get<UpdateStmt>(stmt);
    EXPECT_EQ(update.table, "users");
    EXPECT_EQ(update.assignments.size(), 1u);
}

TEST_F(ParserTest, UpdateMultipleColumns) {
    auto stmt = parser.parse("UPDATE users SET name = 'Bob', age = 30 WHERE id = 1");
    ASSERT_TRUE(std::holds_alternative<UpdateStmt>(stmt));

    const auto& update = std::get<UpdateStmt>(stmt);
    EXPECT_EQ(update.assignments.size(), 2u);
}

// ============================================================================
// DELETE 语句测试
// ============================================================================

TEST_F(ParserTest, SimpleDelete) {
    auto stmt = parser.parse("DELETE FROM users WHERE id = 1");
    ASSERT_TRUE(std::holds_alternative<DeleteStmt>(stmt));

    const auto& del = std::get<DeleteStmt>(stmt);
    EXPECT_EQ(del.table, "users");
    EXPECT_TRUE(del.where.has_value());
}

TEST_F(ParserTest, DeleteAll) {
    auto stmt = parser.parse("DELETE FROM users");
    ASSERT_TRUE(std::holds_alternative<DeleteStmt>(stmt));

    const auto& del = std::get<DeleteStmt>(stmt);
    EXPECT_EQ(del.table, "users");
}

// ============================================================================
// CREATE TABLE 语句测试
// ============================================================================

TEST_F(ParserTest, CreateTable) {
    auto stmt = parser.parse("CREATE TABLE users (id INT64, name TEXT)");
    ASSERT_TRUE(std::holds_alternative<CreateStmt>(stmt));

    const auto& create = std::get<CreateStmt>(stmt);
    EXPECT_EQ(create.table, "users");
    EXPECT_EQ(create.columns.size(), 2u);
}

TEST_F(ParserTest, CreateTableWithManyColumns) {
    auto stmt = parser.parse("CREATE TABLE employees (id INT64, name TEXT, age INT64, department TEXT)");
    ASSERT_TRUE(std::holds_alternative<CreateStmt>(stmt));

    const auto& create = std::get<CreateStmt>(stmt);
    EXPECT_EQ(create.columns.size(), 4u);
}

// ============================================================================
// CREATE INDEX 语句测试
// ============================================================================

TEST_F(ParserTest, CreateIndex) {
    auto stmt = parser.parse("CREATE INDEX idx_name ON users (name)");
    ASSERT_TRUE(std::holds_alternative<CreateIndexStmt>(stmt));

    const auto& create_idx = std::get<CreateIndexStmt>(stmt);
    EXPECT_EQ(create_idx.index_name, "idx_name");
    EXPECT_EQ(create_idx.table, "users");
    EXPECT_EQ(create_idx.column, "name");
}

// ============================================================================
// EXPLAIN 语句测试
// ============================================================================

TEST_F(ParserTest, ExplainSelect) {
    auto stmt = parser.parse("EXPLAIN SELECT * FROM users");
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<ExplainStmt>>(stmt));
}

// ============================================================================
// 表达式解析测试
// ============================================================================

TEST_F(ParserTest, WhereWithAnd) {
    auto stmt = parser.parse("SELECT * FROM users WHERE id > 1 AND name = 'Alice'");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));

    const auto& select = std::get<SelectStmt>(stmt);
    EXPECT_TRUE(select.where.has_value());
}

TEST_F(ParserTest, WhereWithOr) {
    auto stmt = parser.parse("SELECT * FROM users WHERE id = 1 OR id = 2");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

TEST_F(ParserTest, WhereWithComparisonOperators) {
    // 测试各种比较运算符
    parser.parse("SELECT * FROM users WHERE id = 1");
    parser.parse("SELECT * FROM users WHERE id <> 1");
    parser.parse("SELECT * FROM users WHERE id < 10");
    parser.parse("SELECT * FROM users WHERE id <= 10");
    parser.parse("SELECT * FROM users WHERE id > 0");
    parser.parse("SELECT * FROM users WHERE id >= 0");
    SUCCEED();
}

// ============================================================================
// 聚合函数测试
// ============================================================================

TEST_F(ParserTest, CountAggregate) {
    auto stmt = parser.parse("SELECT COUNT(*) FROM users");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

TEST_F(ParserTest, SumAggregate) {
    auto stmt = parser.parse("SELECT SUM(amount) FROM orders");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

TEST_F(ParserTest, AvgAggregate) {
    auto stmt = parser.parse("SELECT AVG(age) FROM users");
    ASSERT_TRUE(std::holds_alternative<SelectStmt>(stmt));
}

TEST_F(ParserTest, MinMaxAggregate) {
    parser.parse("SELECT MIN(price) FROM products");
    parser.parse("SELECT MAX(price) FROM products");
    SUCCEED();
}

// ============================================================================
// 错误处理测试
// ============================================================================

TEST_F(ParserTest, InvalidSyntax) {
    EXPECT_THROW(parser.parse("SELEC * FROM users"), std::runtime_error);
}

TEST_F(ParserTest, MissingTableName) {
    EXPECT_THROW(parser.parse("SELECT * FROM"), std::runtime_error);
}

TEST_F(ParserTest, UnterminatedString) {
    EXPECT_THROW(parser.parse("INSERT INTO users VALUES (1, 'Alice)"), std::runtime_error);
}

TEST_F(ParserTest, EmptyStatement) {
    EXPECT_THROW(parser.parse(""), std::runtime_error);
}

// ============================================================================
// 大小写不敏感测试
// ============================================================================

TEST_F(ParserTest, CaseInsensitiveKeywords) {
    auto stmt1 = parser.parse("select * from users");
    auto stmt2 = parser.parse("SELECT * FROM users");
    auto stmt3 = parser.parse("Select * From Users");

    EXPECT_TRUE(std::holds_alternative<SelectStmt>(stmt1));
    EXPECT_TRUE(std::holds_alternative<SelectStmt>(stmt2));
    EXPECT_TRUE(std::holds_alternative<SelectStmt>(stmt3));
}

// ============================================================================
// 字符串值测试
// ============================================================================

TEST_F(ParserTest, StringWithSpaces) {
    auto stmt = parser.parse("INSERT INTO users VALUES (1, 'Alice Smith')");
    ASSERT_TRUE(std::holds_alternative<InsertStmt>(stmt));
}

TEST_F(ParserTest, NumericValues) {
    // 注意: 当前解析器不支持负数字面量
    auto stmt = parser.parse("INSERT INTO data VALUES (123, 456, 0)");
    ASSERT_TRUE(std::holds_alternative<InsertStmt>(stmt));
}

// ============================================================================
// ColumnRef 测试
// ============================================================================

TEST(ColumnRefTest, SimpleColumnRef) {
    ColumnRef ref{ "", "name" };
    EXPECT_EQ(ref.qualified_name(), "name");
}

TEST(ColumnRefTest, QualifiedColumnRef) {
    ColumnRef ref{ "users", "name" };
    EXPECT_EQ(ref.qualified_name(), "users.name");
}

TEST(ColumnRefTest, ColumnRefMatches) {
    ColumnRef ref1{ "users", "name" };
    ColumnRef ref2{ "", "name" };
    ColumnRef ref3{ "orders", "name" };

    EXPECT_TRUE(ref1.matches(ref2));  // 无表名限定可以匹配
    EXPECT_FALSE(ref1.matches(ref3)); // 不同表名不匹配
}

TEST(ColumnRefTest, ColumnRefEquality) {
    ColumnRef ref1{ "users", "name" };
    ColumnRef ref2{ "users", "name" };
    ColumnRef ref3{ "users", "id" };

    EXPECT_EQ(ref1, ref2);
    EXPECT_NE(ref1, ref3);
}

// ============================================================================
// Literal 测试
// ============================================================================

TEST(LiteralTest, IntLiteral) {
    auto lit = Literal::from_int(42);
    EXPECT_TRUE(std::holds_alternative<int64_t>(lit.value));
    EXPECT_EQ(std::get<int64_t>(lit.value), 42);
}

TEST(LiteralTest, StringLiteral) {
    auto lit = Literal::from_string("hello");
    EXPECT_TRUE(std::holds_alternative<std::string>(lit.value));
    EXPECT_EQ(std::get<std::string>(lit.value), "hello");
}

TEST(LiteralTest, NullLiteral) {
    auto lit = Literal::null();
    EXPECT_TRUE(lit.is_null());
    EXPECT_TRUE(std::holds_alternative<NullValue>(lit.value));
}

// ============================================================================
// AggFunc 测试
// ============================================================================

TEST(AggFuncTest, AggFuncName) {
    EXPECT_STREQ(agg_func_name(AggFunc::Count), "COUNT");
    EXPECT_STREQ(agg_func_name(AggFunc::Sum), "SUM");
    EXPECT_STREQ(agg_func_name(AggFunc::Avg), "AVG");
    EXPECT_STREQ(agg_func_name(AggFunc::Min), "MIN");
    EXPECT_STREQ(agg_func_name(AggFunc::Max), "MAX");
}
