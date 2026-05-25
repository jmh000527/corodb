// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file bool_evaluator.h
 *  @brief 布尔/比较表达式求值器（三值逻辑）。
 */

#pragma once

#include "corodb/ast/ast.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief 布尔与比较表达式求值器（SQL 三值逻辑）。
     */
    class BoolEvaluator {
    public:
        /** @brief NULLS LAST 顺序比较；返回 -1/0/1。 */
        [[nodiscard]] static int compare_order(const Value& lhs, const Value& rhs);

        /** @brief 求值比较表达式。 */
        [[nodiscard]] static SqlBool eval_comparison(const Comparison& cmp, const Record& record);

        /** @brief 求值布尔表达式（AND/OR/NOT/IN/BETWEEN）。 */
        [[nodiscard]] static SqlBool eval(const BoolExpr& expr, const Record& record);
    };

} // namespace corodb
