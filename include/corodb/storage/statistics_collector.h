// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file statistics_collector.h @brief 统计信息采集器（ANALYZE 核心逻辑）。 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "corodb/common/types.h"
#include "corodb/storage/statistics.h"

namespace corodb {

    class Table;

    /** @brief 统计采集器：对表执行采样并计算列级统计。 */
    class StatisticsCollector {
    public:
        /** @brief 采集配置参数。 */
        struct Config {
            std::size_t sample_target{ 300 };     ///< 每列目标采样行数
            std::size_t max_mcv{ 20 };            ///< MCV 最多保留个数
            std::size_t histogram_buckets{ 32 };  ///< 直方图桶数
            double      mcv_threshold{ 1.25 };    ///< MCV 频率阈值倍数（vs 1/ndistinct）
        };

        explicit StatisticsCollector(Config cfg = {}) : cfg_(std::move(cfg)) {}

        /** @brief 对单表采集统计信息。
         *  @param table 目标表。
         *  @param snapshot_ts MVCC 快照时间戳（同时用作 stats_ts）。
         *  @return 表级统计集合。 */
        [[nodiscard]] TableStats collect(const Table& table, uint64_t snapshot_ts) const;

    private:
        Config cfg_;

        /** @brief 单列采集：从样本值计算完整 ColumnStats。 */
        [[nodiscard]] ColumnStats build_column_stats(
            const std::string& col_name, TypeKind col_type,
            const std::vector<Value>& samples,
            std::size_t null_count, std::size_t total_rows,
            uint64_t stats_ts) const;

        /** @brief 计算 MCV（最常用值）。
         *  @param samples 非_NULL 样本值（将被修改：移除 MCV 后剩余值供直方图使用）。
         *  @param ndistinct 去重值数。
         *  @param total_rows 表总行数（用于频率外推）。
         *  @param[out] mcv_values 高频值列表。
         *  @param[out] mcv_freqs 对应频率列表。 */
        void compute_mcv(std::vector<Value>& samples, std::size_t ndistinct,
                         std::size_t total_rows,
                         std::vector<Value>& mcv_values,
                         std::vector<double>& mcv_freqs) const;

        /** @brief 计算等高直方图边界。
         *  @param remaining 排除 MCV 后的值（将被排序）。
         *  @param[out] bounds 桶边界（n_buckets+1 个值）。 */
        void compute_histogram(std::vector<Value>& remaining,
                               std::vector<Value>& bounds) const;

        /** @brief 估算 NDV（去重值数）。
         *  @param sample_distinct 样本中去重值数。
         *  @param sample_size 样本大小。
         *  @param total_rows 表总行数。
         *  @return 估算的 NDV。 */
        [[nodiscard]] std::size_t estimate_ndiv(std::size_t sample_distinct,
                                                 std::size_t sample_size,
                                                 std::size_t total_rows) const;
    };

} // namespace corodb
