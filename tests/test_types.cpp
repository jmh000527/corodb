/**
 * @file test_types.cpp
 * @brief 核心类型单元测试
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 测试 corodb/common/types.h 中定义的核心类型
 */

#include <gtest/gtest.h>
#include <sstream>

#include "corodb/common/types.h"

using namespace corodb;

// ============================================================================
// NullValue 测试
// ============================================================================

class NullValueTest : public ::testing::Test {
protected:
    NullValue null1;
    NullValue null2;
};

TEST_F(NullValueTest, DefaultConstruction) {
    NullValue nv;
    // NullValue 应该可以默认构造
    SUCCEED();
}

TEST_F(NullValueTest, EqualityComparison) {
    EXPECT_EQ(null1, null2);
    EXPECT_FALSE(null1 != null2);
}

TEST_F(NullValueTest, OutputStreamOperator) {
    std::ostringstream oss;
    oss << null1;
    EXPECT_EQ(oss.str(), "NULL");
}

// ============================================================================
// Value 类型测试
// ============================================================================

class ValueTest : public ::testing::Test {
protected:
    Value null_value = NullValue{};
    Value int_value = int64_t{ 42 };
    Value string_value = std::string{ "Hello, CoroDB!" };
    Value negative_int = int64_t{ -100 };
    Value zero_value = int64_t{ 0 };
    Value empty_string = std::string{ "" };
};

TEST_F(ValueTest, NullValueHoldsCorrectType) {
    EXPECT_TRUE(std::holds_alternative<NullValue>(null_value));
    EXPECT_FALSE(std::holds_alternative<int64_t>(null_value));
    EXPECT_FALSE(std::holds_alternative<std::string>(null_value));
}

TEST_F(ValueTest, IntValueHoldsCorrectType) {
    EXPECT_TRUE(std::holds_alternative<int64_t>(int_value));
    EXPECT_FALSE(std::holds_alternative<NullValue>(int_value));
    EXPECT_FALSE(std::holds_alternative<std::string>(int_value));
}

TEST_F(ValueTest, StringValueHoldsCorrectType) {
    EXPECT_TRUE(std::holds_alternative<std::string>(string_value));
    EXPECT_FALSE(std::holds_alternative<NullValue>(string_value));
    EXPECT_FALSE(std::holds_alternative<int64_t>(string_value));
}

TEST_F(ValueTest, GetIntValue) {
    EXPECT_EQ(std::get<int64_t>(int_value), 42);
    EXPECT_EQ(std::get<int64_t>(negative_int), -100);
    EXPECT_EQ(std::get<int64_t>(zero_value), 0);
}

TEST_F(ValueTest, GetStringValue) {
    EXPECT_EQ(std::get<std::string>(string_value), "Hello, CoroDB!");
    EXPECT_EQ(std::get<std::string>(empty_string), "");
}

TEST_F(ValueTest, BadVariantAccess) {
    EXPECT_THROW(std::get<int64_t>(string_value), std::bad_variant_access);
    EXPECT_THROW(std::get<std::string>(int_value), std::bad_variant_access);
    EXPECT_THROW(std::get<int64_t>(null_value), std::bad_variant_access);
}

// ============================================================================
// TypeKind 测试
// ============================================================================

class TypeKindTest : public ::testing::Test {};

TEST_F(TypeKindTest, TypeNameFunction) {
    EXPECT_EQ(type_name(TypeKind::Int64), "int64");
    EXPECT_EQ(type_name(TypeKind::Text), "text");
    EXPECT_EQ(type_name(TypeKind::Null), "null");
}

TEST_F(TypeKindTest, ValueTypeFunction) {
    Value null_v = NullValue{};
    Value int_v = int64_t{ 123 };
    Value str_v = std::string{ "test" };

    EXPECT_EQ(value_type(null_v), TypeKind::Null);
    EXPECT_EQ(value_type(int_v), TypeKind::Int64);
    EXPECT_EQ(value_type(str_v), TypeKind::Text);
}

// ============================================================================
// Oid 测试
// ============================================================================

class OidTest : public ::testing::Test {};

TEST_F(OidTest, InvalidOidConstant) {
    EXPECT_EQ(INVALID_OID, 0u);
}

TEST_F(OidTest, OidIsUint32) {
    Oid oid = 12345;
    EXPECT_EQ(oid, 12345u);

    // 测试边界值
    Oid max_oid = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(max_oid, 0xFFFFFFFF);
}

// ============================================================================
// is_null 帮助函数测试 (如果存在)
// ============================================================================

TEST(ValueHelperTest, IsNullCheck) {
    Value null_v = NullValue{};
    Value int_v = int64_t{ 0 };
    Value str_v = std::string{ "" };

    EXPECT_TRUE(std::holds_alternative<NullValue>(null_v));
    EXPECT_FALSE(std::holds_alternative<NullValue>(int_v));
    EXPECT_FALSE(std::holds_alternative<NullValue>(str_v));
}

// ============================================================================
// Value 拷贝和移动语义测试
// ============================================================================

class ValueSemanticsTest : public ::testing::Test {};

TEST_F(ValueSemanticsTest, CopyConstruction) {
    Value original = std::string{ "test string" };
    Value copy = original;

    EXPECT_EQ(std::get<std::string>(copy), "test string");
    EXPECT_EQ(std::get<std::string>(original), "test string");
}

TEST_F(ValueSemanticsTest, MoveConstruction) {
    Value original = std::string{ "test string" };
    Value moved = std::move(original);

    EXPECT_EQ(std::get<std::string>(moved), "test string");
    // original 的状态未定义，但应该仍然是有效的 variant
}

TEST_F(ValueSemanticsTest, CopyAssignment) {
    Value v1 = int64_t{ 42 };
    Value v2 = std::string{ "hello" };

    v2 = v1;

    EXPECT_TRUE(std::holds_alternative<int64_t>(v2));
    EXPECT_EQ(std::get<int64_t>(v2), 42);
}

TEST_F(ValueSemanticsTest, MoveAssignment) {
    Value v1 = std::string{ "long string for testing move semantics" };
    Value v2 = NullValue{};

    v2 = std::move(v1);

    EXPECT_TRUE(std::holds_alternative<std::string>(v2));
    EXPECT_EQ(std::get<std::string>(v2), "long string for testing move semantics");
}

// ============================================================================
// 边界值测试
// ============================================================================

class ValueBoundaryTest : public ::testing::Test {};

TEST_F(ValueBoundaryTest, MaxInt64Value) {
    Value max_int = std::numeric_limits<int64_t>::max();
    EXPECT_EQ(std::get<int64_t>(max_int), std::numeric_limits<int64_t>::max());
}

TEST_F(ValueBoundaryTest, MinInt64Value) {
    Value min_int = std::numeric_limits<int64_t>::min();
    EXPECT_EQ(std::get<int64_t>(min_int), std::numeric_limits<int64_t>::min());
}

TEST_F(ValueBoundaryTest, LargeStringValue) {
    std::string large_str(10000, 'x');
    Value v = large_str;
    EXPECT_EQ(std::get<std::string>(v).size(), 10000u);
}

TEST_F(ValueBoundaryTest, UnicodeStringValue) {
    Value v = std::string{ "你好世界🌍" };
    EXPECT_EQ(std::get<std::string>(v), "你好世界🌍");
}
