// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file explain_printer.h
 *  @brief EXPLAIN 输出格式化器（PG 风格双段输出）。
 */

#pragma once

#include <generator>
#include <string>

#include "corodb/plan/physical_plan.h"
#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief 将物理/逻辑计划格式化为 PostgreSQL 风格的 EXPLAIN 文本。
     */
    class ExplainPrinter {
    public:
        /** @brief 仅生成物理计划文本。 */
        [[nodiscard]] static std::string format(const PlanNode* physical);

        /** @brief 物理计划单段表格行流。 */
        [[nodiscard]] static std::generator<Record> render(const PlanNode* physical);

        /** @brief 逻辑 + 物理双段表格行流。 */
        [[nodiscard]] static std::generator<Record> render_dual(std::string logical_text, const PlanNode* physical);

        /** @brief EXPLAIN ANALYZE: 计划 + 运行时统计。 */
        [[nodiscard]] static std::generator<Record> render_analyze(std::string logical_text,
                                                                   const PlanNode* physical,
                                                                   const QueryStats& stats);
    };

} // namespace corodb
