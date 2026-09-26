// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file statistics_collector.cpp
// @brief 统计信息采集器实现。
//
// 采集流程：
//   1. 估算表行数，决定采样策略（全量 vs Bernoulli 采样）
//   2. 单遍流式扫描（scan_visible_stream），对每行做 Bernoulli 抽样
//   3. 选中行的各列值分别收集到 per-column 样本向量
//   4. 扫描结束后，对每列计算 NDV / MCV / 直方图 / null_frac

#include "corodb/storage/statistics_collector.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "corodb/storage/table.h"

namespace corodb {

    namespace {

        /// 固定随机种子，保证 ANALYZE 结果可复现（便于测试）。
        constexpr std::uint64_t kRngSeed = 42;

        /// 判断 TypeKind 是否可排序（可建直方图）。
        bool is_sortable(TypeKind kind) noexcept {
            switch (kind) {
                case TypeKind::Int64:
                case TypeKind::Text:
                case TypeKind::Float64:
                case TypeKind::Date:
                case TypeKind::Decimal:
                    return true;
                default:
                    return false;
            }
        }

    } // anonymous namespace

    // =========================================================================
    // collect — 入口
    // =========================================================================

    TableStats StatisticsCollector::collect(const Table& table, uint64_t snapshot_ts) const {
        const auto& columns = table.columns();
        const std::size_t ncols = columns.size();
        const std::size_t estimated = table.estimated_row_count();

        TableStats result;
        result.table_name = table.name();
        result.stats_ts = snapshot_ts;

        if (ncols == 0)
            return result;

        // --- 采样策略 ---
        // 小表（估算行数 ≤ sample_target × 3）：全量扫描
        // 大表：Bernoulli 采样，每行以概率 p 纳入样本
        const bool full_scan = (estimated <= cfg_.sample_target * 3);
        const double sample_rate =
            full_scan ? 1.0
                      : static_cast<double>(cfg_.sample_target) /
                            static_cast<double>(estimated > 0 ? estimated : 1);

        // --- Per-column 采集缓冲 ---
        std::vector<std::size_t> null_counts(ncols, 0);
        std::vector<std::vector<Value>> col_samples(ncols);
        // 预留空间避免频繁 realloc
        if (!full_scan) {
            for (auto& s : col_samples)
                s.reserve(cfg_.sample_target + 16);
        }

        // --- 单遍流式扫描 + Bernoulli 采样 ---
        std::mt19937_64 rng(kRngSeed);
        std::size_t scanned = 0;

        for (auto&& row : table.scan_visible_stream(snapshot_ts)) {
            ++scanned;
            // Bernoulli 判定：每行只做一次随机决策（所有列同步采样）
            const bool selected = full_scan || sample_rate >= 1.0 ||
                                  static_cast<double>(rng()) /
                                          static_cast<double>(rng.max()) <
                                      sample_rate;
            if (!selected)
                continue;

            const std::size_t nvals = std::min(row.values.size(), ncols);
            for (std::size_t i = 0; i < nvals; ++i) {
                if (is_null(row.values[i])) {
                    ++null_counts[i];
                } else {
                    col_samples[i].push_back(row.values[i]);
                }
            }
        }

        result.total_rows = scanned;

        // --- 逐列构建 ColumnStats ---
        for (std::size_t i = 0; i < ncols; ++i) {
            auto stats = build_column_stats(
                columns[i].name, columns[i].type,
                col_samples[i], null_counts[i], scanned, snapshot_ts);
            result.columns[columns[i].name] = std::move(stats);
        }

        return result;
    }

    // =========================================================================
    // build_column_stats — 单列统计计算
    // =========================================================================

    ColumnStats StatisticsCollector::build_column_stats(
        const std::string& col_name, TypeKind col_type,
        const std::vector<Value>& samples,
        std::size_t null_count, std::size_t total_rows,
        uint64_t stats_ts) const {

        ColumnStats stats;
        stats.column_name = col_name;
        stats.type = col_type;
        stats.total_rows = total_rows;
        stats.null_count = null_count;
        stats.null_frac = total_rows > 0
                              ? static_cast<double>(null_count) / static_cast<double>(total_rows)
                              : 0.0;
        stats.stats_ts = stats_ts;

        const std::size_t sample_size = samples.size();
        if (sample_size == 0) {
            // 全 NULL 或空表
            stats.ndistinct = 0;
            return stats;
        }

        // --- NDV 计算 ---
        // 统计样本中去重值数
        std::unordered_map<Value, std::size_t, ValueHash, ValueEq> freq_map;
        freq_map.reserve(sample_size * 2);
        for (const auto& v : samples) {
            ++freq_map[v];
        }
        const std::size_t sample_distinct = freq_map.size();
        stats.ndistinct = estimate_ndiv(sample_distinct, sample_size, total_rows);

        // --- MCV 计算 ---
        // 需要可修改的样本副本（MCV 计算会移除已选值，剩余供直方图使用）
        std::vector<Value> remaining(samples);
        compute_mcv(remaining, stats.ndistinct, total_rows,
                    stats.mcv_values, stats.mcv_freqs);

        // --- 直方图计算 ---
        if (is_sortable(col_type) && !remaining.empty()) {
            compute_histogram(remaining, stats.histogram_bounds);
        }

        // --- correlation（Phase 1 暂不采集，留默认 0.0） ---
        stats.correlation = 0.0;

        return stats;
    }

    // =========================================================================
    // compute_mcv — 最常用值
    // =========================================================================

    void StatisticsCollector::compute_mcv(
        std::vector<Value>& samples, std::size_t ndistinct,
        std::size_t total_rows,
        std::vector<Value>& mcv_values,
        std::vector<double>& mcv_freqs) const {

        mcv_values.clear();
        mcv_freqs.clear();

        if (samples.empty() || ndistinct == 0)
            return;

        // 频率统计
        std::unordered_map<Value, std::size_t, ValueHash, ValueEq> freq_map;
        freq_map.reserve(samples.size() * 2);
        for (const auto& v : samples) {
            ++freq_map[v];
        }

        // 平均频率 = 1 / ndistinct
        const double avg_freq = ndistinct > 0
                                    ? 1.0 / static_cast<double>(ndistinct)
                                    : 1.0;
        const double threshold = cfg_.mcv_threshold * avg_freq;

        // MCV 频率相对于全表行数（含 NULL），与 null_frac 互补
        const double total_non_null = static_cast<double>(
            total_rows > 0 ? total_rows : samples.size());

        // 筛选超过阈值的值，按频率降序排列
        std::vector<std::pair<Value, std::size_t>> candidates;
        candidates.reserve(freq_map.size());
        for (const auto& [val, cnt] : freq_map) {
            double freq = static_cast<double>(cnt) / total_non_null;
            if (freq > threshold) {
                candidates.emplace_back(val, cnt);
            }
        }

        // 降序排列
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // 截取 max_mcv 个
        const std::size_t keep = std::min(candidates.size(), cfg_.max_mcv);
        mcv_values.reserve(keep);
        mcv_freqs.reserve(keep);
        for (std::size_t i = 0; i < keep; ++i) {
            mcv_values.push_back(candidates[i].first);
            mcv_freqs.push_back(static_cast<double>(candidates[i].second) / total_non_null);
        }

        // 从 samples 中移除 MCV 值，剩余供直方图使用
        if (!mcv_values.empty()) {
            // 构建查找集合
            std::unordered_map<Value, bool, ValueHash, ValueEq> mcv_set;
            for (const auto& v : mcv_values) {
                mcv_set[v] = true;
            }
            // stable_remove：保留非 MCV 值的相对顺序
            auto new_end = std::remove_if(samples.begin(), samples.end(),
                                          [&mcv_set](const Value& v) {
                                              return mcv_set.count(v) > 0;
                                          });
            samples.erase(new_end, samples.end());
        }
    }

    // =========================================================================
    // compute_histogram — 等高直方图
    // =========================================================================

    void StatisticsCollector::compute_histogram(
        std::vector<Value>& remaining,
        std::vector<Value>& bounds) const {

        bounds.clear();
        const std::size_t n = remaining.size();
        if (n == 0)
            return;

        // 排序
        std::sort(remaining.begin(), remaining.end(), ValueLess{});

        // 桶数：取 min(configured, n/2)——每桶至少 2 个值才有意义
        const std::size_t max_buckets = std::min(cfg_.histogram_buckets, n / 2);
        if (max_buckets < 2) {
            // 数据量太少，只记录 min 和 max
            bounds.push_back(remaining.front());
            bounds.push_back(remaining.back());
            return;
        }

        // 等高直方图：每个桶包含约 n/buckets 个值
        // 边界：bounds[0]=min, bounds[buckets]=max, 中间各桶分界点
        bounds.reserve(max_buckets + 1);
        bounds.push_back(remaining.front()); // bounds[0] = min

        for (std::size_t b = 1; b < max_buckets; ++b) {
            std::size_t idx = b * n / max_buckets;
            if (idx >= n)
                idx = n - 1;
            bounds.push_back(remaining[idx]);
        }

        bounds.push_back(remaining.back()); // bounds[max_buckets] = max
    }

    // =========================================================================
    // estimate_ndiv — NDV 估算
    // =========================================================================

    std::size_t StatisticsCollector::estimate_ndiv(
        std::size_t sample_distinct, std::size_t sample_size,
        std::size_t total_rows) const {

        if (sample_size == 0)
            return 0;

        // 全量扫描（sample_size ≈ total_rows）：直接返回精确值
        if (sample_size >= total_rows) {
            return sample_distinct;
        }

        // 采样外推：未见到的值中仍有去重值
        // 估计：ndistinct ≈ sample_distinct + (total - sample) × (未见值比例)
        // 未见值比例 = 1 - sample_distinct / sample_size（样本中每个值平均出现 sample_size/distinct 次）
        const double unseen_ratio =
            1.0 - static_cast<double>(sample_distinct) / static_cast<double>(sample_size);
        const std::size_t remaining_rows = total_rows > sample_size
                                               ? total_rows - sample_size
                                               : 0;
        const std::size_t estimated =
            sample_distinct +
            static_cast<std::size_t>(static_cast<double>(remaining_rows) * unseen_ratio);

        // 上限：不超过总行数
        return std::min(estimated, total_rows);
    }

} // namespace corodb
