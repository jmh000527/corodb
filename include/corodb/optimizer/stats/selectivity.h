// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file selectivity.h @brief 基于 ANALYZE 统计（MCV + 等高直方图 + NDV）的谓词选择率估计（T1.5）。 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "corodb/ast/ast.h"
#include "corodb/common/types.h"
#include "corodb/storage/statistics.h"
#include "corodb/storage/table.h"

namespace corodb::opt {

    /**
     * @brief 选择率估计器：把 ColumnStats（MCV 频率、等高直方图、NDV、null_frac）
     *        转换为谓词选择率，供物理规划器与 Join 重排做基数估算。
     *
     * 归一化约定（与 StatisticsCollector 一致）：
     *   - MCV 频率相对全表行数（含 NULL）；
     *   - 直方图只覆盖非 MCV、非 NULL 的值，行占比 = 1 - null_frac - Σmcv；
     *   - 等高直方图每桶代表 (1/n_buckets) 的直方图人口。
     *
     * 无统计时回退固定默认值（kDefaultEq/Range），保证规划永不失败。
     */
    class SelectivityEstimator {
    public:
        /// 无统计时等值谓词默认选择率（PHASE1_TASK_LIST T1.5 约定）。
        static constexpr double kDefaultEqSelectivity = 0.005;
        /// 无统计时范围谓词默认选择率。
        static constexpr double kDefaultRangeSelectivity = 0.333;

        // ------------------------------------------------------------------
        // 单谓词（列级）
        // ------------------------------------------------------------------

        /** @brief col = v 的选择率。
         *
         * MCV 命中 → 精确频率；未命中 → 非 MCV 非 NULL 行均摊到其余 NDV；
         * 无统计 → kDefaultEqSelectivity。下限 1/total_rows（保证行数 ≥1）。 */
        [[nodiscard]] static double selectivity_eq(const ColumnStats* stats, const Value& value);

        /** @brief col != v 的选择率（1 - eq）。 */
        [[nodiscard]] static double selectivity_ne(const ColumnStats* stats, const Value& value);

        /** @brief 范围 [low, high]（按 inclusive 标志，nullopt 表示该侧无界）的选择率。
         *
         * MCV 贡献（落在范围内的 MCV 频率求和）+ 直方图贡献（边界桶内线性插值 ×
         * 直方图人口占比）；无直方图时直方图部分回退 kDefaultRangeSelectivity。 */
        [[nodiscard]] static double selectivity_range(const ColumnStats* stats,
                                                      const std::optional<Value>& low, bool low_inclusive,
                                                      const std::optional<Value>& high, bool high_inclusive);

        /** @brief col BETWEEN low AND high（闭区间）。 */
        [[nodiscard]] static double selectivity_between(const ColumnStats* stats, const Value& low,
                                                        const Value& high);

        /** @brief col IN (v1..vn) 的选择率（Σ去重等值，封顶 1）。 */
        [[nodiscard]] static double selectivity_in_list(const ColumnStats* stats,
                                                        const std::vector<Value>& values);

        /** @brief col IS NULL 的选择率（null_frac；无统计视为 0）。 */
        [[nodiscard]] static double selectivity_is_null(const ColumnStats* stats);

        // ------------------------------------------------------------------
        // 组合
        // ------------------------------------------------------------------

        /** @brief AND 组合：独立乘积。 */
        [[nodiscard]] static double combine_and(double a, double b) noexcept;

        /** @brief OR 组合：容斥 a + b - a·b。 */
        [[nodiscard]] static double combine_or(double a, double b) noexcept;

        // ------------------------------------------------------------------
        // 上层入口
        // ------------------------------------------------------------------

        /** @brief 估计整个 WHERE 谓词在给定表上的选择率。
         *
         * 递归拆解 BoolExpr：AND 乘积、OR 容斥、NOT 取补；比较叶归一化为
         * col OP literal 后查列统计；无法解析的叶回退 kDefaultRangeSelectivity。 */
        [[nodiscard]] static double estimate_filter_selectivity(const Table& table,
                                                                const BoolExpr& predicate);
    };

} // namespace corodb::opt
