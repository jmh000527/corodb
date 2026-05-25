// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file types.cpp
// @brief 类型系统辅助函数（type_name / value_type / generate_oid）实现。

#include "corodb/common/types.h"

#include <atomic>

namespace corodb {

    /**
     * @brief 返回 TypeKind 枚举对应的类型名称字符串。
     */
    std::string type_name(TypeKind kind) noexcept {
        switch (kind) {
            case TypeKind::Int64:
                return "int64";
            case TypeKind::Text:
                return "text";
            case TypeKind::Float64:
                return "float64";
            case TypeKind::Null:
                return "null";
        }
        return "unknown";
    }

    /**
     * @brief 返回 Value 变体实际持有的类型种类。
     */
    TypeKind value_type(const Value& v) noexcept {
        if (std::holds_alternative<NullValue>(v))
            return TypeKind::Null;
        if (std::holds_alternative<int64_t>(v))
            return TypeKind::Int64;
        if (std::holds_alternative<double>(v))
            return TypeKind::Float64;
        return TypeKind::Text;
    }

    /**
     * @brief 生成全局唯一的对象 ID（单调递增，从 1 开始）。
     */
    Oid generate_oid() noexcept {
        static std::atomic<Oid> next_oid{ 1 };
        return next_oid.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace corodb
