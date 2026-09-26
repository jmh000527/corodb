// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file statistics.h @brief 列级统计信息结构定义（ANALYZE 采集）。 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "corodb/common/types.h"

namespace corodb {

    /** @brief 单列统计信息（ANALYZE 采集）。 */
    struct ColumnStats {
        // --- 基础 ---
        std::string column_name;        ///< 列名
        TypeKind    type{ TypeKind::Null }; ///< 列类型
        std::size_t total_rows{ 0 };    ///< 采样时表总行数
        std::size_t null_count{ 0 };    ///< NULL 值数
        double      null_frac{ 0.0 };   ///< null_count / total_rows

        // --- 基数 ---
        std::size_t ndistinct{ 0 };     ///< 去重值数（精确或估算）

        // --- 最常用值 (MCV) ---
        std::vector<Value>  mcv_values; ///< 高频值（按频率降序排列）
        std::vector<double> mcv_freqs;  ///< 对应频率 (0..1]，与 mcv_values 一一对应

        // --- 等高直方图 ---
        /// 排除 MCV 后的值按值域分桶，每桶代表约等量行。
        /// 桶边界，n_buckets+1 个值；空 = 无直方图（不可排序类型或数据量不足）。
        std::vector<Value> histogram_bounds;

        // --- 物理相关 ---
        double correlation{ 0.0 };      ///< 物理顺序与列值序的相关性 [-1,1]；影响 IndexScan 代价
                                        ///< Phase 1 采集但不使用，Phase 2 代价模型消费

        // --- 元信息 ---
        uint64_t stats_ts{ 0 };         ///< 采集时的事务时间戳（用于判断是否陈旧）

        /** @brief 是否有有效统计。 */
        [[nodiscard]] bool valid() const noexcept {
            return total_rows > 0 || stats_ts > 0;
        }

        /** @brief 统计是否陈旧（行数变化超阈值）。 */
        [[nodiscard]] bool is_stale(std::size_t current_rows, double threshold = 0.1) const noexcept {
            if (total_rows == 0)
                return true;
            double change = std::abs(static_cast<double>(current_rows) - static_cast<double>(total_rows));
            return change / static_cast<double>(total_rows) > threshold;
        }

        /** @brief 序列化到二进制字符串（用于持久化到 .stats 文件）。 */
        [[nodiscard]] std::string serialize() const;

        /** @brief 从二进制字符串反序列化。 */
        static std::optional<ColumnStats> deserialize(const std::string& data);
    };

    /** @brief 单表全部列的统计集合。 */
    struct TableStats {
        std::string table_name;         ///< 表名
        std::size_t total_rows{ 0 };    ///< 采集时表总行数
        uint64_t    stats_ts{ 0 };      ///< 采集时间戳

        /// 列名 → 列统计
        std::unordered_map<std::string, ColumnStats> columns;

        /** @brief 获取指定列的统计信息；无统计返回 nullptr。 */
        [[nodiscard]] const ColumnStats* column(const std::string& name) const {
            auto it = columns.find(name);
            return it != columns.end() ? &it->second : nullptr;
        }

        /** @brief 是否有有效统计。 */
        [[nodiscard]] bool valid() const noexcept {
            return total_rows > 0 && !columns.empty();
        }

        /** @brief 统计是否陈旧。 */
        [[nodiscard]] bool is_stale(std::size_t current_rows, double threshold = 0.1) const noexcept {
            if (total_rows == 0)
                return true;
            double change = std::abs(static_cast<double>(current_rows) - static_cast<double>(total_rows));
            return change / static_cast<double>(total_rows) > threshold;
        }

        /** @brief 序列化到二进制字符串。 */
        [[nodiscard]] std::string serialize() const;

        /** @brief 从二进制字符串反序列化。 */
        static std::optional<TableStats> deserialize(const std::string& data);
    };

} // namespace corodb
