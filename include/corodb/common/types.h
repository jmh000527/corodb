// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file types.h @brief CoroDB 核心值类型定义。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string>
#include <variant>

/**
 * @brief SQL 三值布尔类型（Three-Valued Logic）。
 *
 * 实现 SQL 标准的三值逻辑，涉及 NULL 的比较结果为 Unknown。
 */
enum class SqlBool {
    True,   ///< 布尔真值
    False,  ///< 布尔假值
    Unknown ///< 未知值（涉及NULL的比较结果）
};

/** @brief 表示 SQL NULL 值的标记类型。 */
struct NullValue {
    constexpr NullValue() noexcept = default;

    constexpr bool operator==(const NullValue& /*other*/) const noexcept {
        return true;
    }

    constexpr bool operator!=(const NullValue& /*other*/) const noexcept {
        return false;
    }
};

/** @brief 为 NullValue 提供 std::hash 特化（支持 unordered_map 索引）。 */
template<>
struct std::hash<NullValue> {
    std::size_t operator()(const NullValue&) const noexcept { return 0; }
};

/** @brief 输出 NullValue（输出字符串 "NULL"）。 */
inline std::ostream& operator<<(std::ostream& os, const NullValue& /*null_value*/) {
    os << "NULL";
    return os;
}

namespace corodb {

    /**
     * @defgroup CoreTypes 核心类型
     * @brief 数据库核心类型定义
     * @{
     */

    /** @brief 对象标识符类型（Object Identifier）。 */
    using Oid = uint32_t;

    /** @brief 无效的 Oid 常量。 */
    constexpr Oid INVALID_OID = 0;

    /** @brief 范围表索引类型（Range Table Index）。 */
    using RtIndex = uint32_t;

    /** @brief 数据库支持的基本数据类型枚举。 */
    enum class TypeKind : uint8_t {
        Int64 = 0,    ///< 64位有符号整数类型
        Text = 1,     ///< 文本字符串类型，UTF-8编码
        Null = 2,     ///< NULL值类型
        Float64 = 3,  ///< IEEE 754 双精度浮点数类型
        Boolean = 4,  ///< 布尔类型（存储为 int64 0/1，域校验拒绝其他值）
        Date = 5,     ///< 日期类型（ISO-8601 字符串，域校验字典序即时间序）
        Decimal = 6   ///< 精确小数（存储为 double，未来升级为定点）
    };

    /** @brief 通用值类型，可存储 NULL、int64_t、double 或 std::string。 */
    using Value = std::variant<NullValue, int64_t, double, std::string>;

    /** @brief 获取 TypeKind 对应的字符串名称。 */
    [[nodiscard]] std::string type_name(TypeKind kind) noexcept;

    /** @brief 从 Value 获取对应的 TypeKind。 */
    [[nodiscard]] TypeKind value_type(const Value& v) noexcept;

    /** @brief 检查 Value 是否为 NULL。 */
    [[nodiscard]] inline bool is_null(const Value& v) noexcept {
        return std::holds_alternative<NullValue>(v);
    }

    /** @brief 线程安全地生成递增的唯一对象标识符。 */
    [[nodiscard]] Oid generate_oid() noexcept;

    /** @} */ // end of CoreTypes group

} // namespace corodb
