// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file expression_evaluator.h
 *  @brief 标量表达式求值器（对应 PostgreSQL 的 execExpr.c）。
 */

#pragma once

#include "corodb/ast/ast.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief 标量表达式求值器。
     *
     * 提供静态方法对 ColumnRef / Literal / BinaryExpr / FunctionExpr / AggregateExpr
     * 等表达式求值。所有方法纯函数（不依赖会话状态），可在任意线程使用。
     */
    class ExpressionEvaluator {
    public:
        /** @brief 根据列引用查找记录中的值；找不到列时抛 std::runtime_error。 */
        [[nodiscard]] static const Value& lookup(const Record& record, const ColumnRef& ref);

        /** @brief 根据列名（忽略表名）查找；找不到抛异常。 */
        [[nodiscard]] static const Value& lookup_by_name(const Record& record, const std::string& name);

        /** @brief 在记录上下文中求值表达式。 */
        [[nodiscard]] static Value eval(const Record& record, const Expression& expr);

        /** @brief 求值标量函数（COALESCE/UPPER/LENGTH 等）。 */
        [[nodiscard]] static Value eval_function(const Record& record, const FunctionExpr& func);
    };

} // namespace corodb
