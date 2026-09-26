// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file cost_model.cpp
// @brief 统一代价模型实现（Phase 2，G1）。

#include "corodb/optimizer/cost/cost_model.h"

#include <algorithm>
#include <cmath>

namespace corodb::opt {

    CostModel::Params CostModel::Params::from_config() {
        const Config& c = Config::instance();
        Params p;
        p.seq_page_cost = c.seq_page_cost();
        p.random_page_cost = c.random_page_cost();
        p.cpu_tuple_cost = c.cpu_tuple_cost();
        p.cpu_index_tuple_cost = c.cpu_index_tuple_cost();
        p.cpu_operator_cost = c.cpu_operator_cost();
        return p;
    }

    // =========================================================================
    // 扫描
    // =========================================================================

    Cost CostModel::seq_scan(double rows) const noexcept {
        Cost c;
        c.startup = 0.0;
        c.total = std::max(rows, 0.0) * (params_.seq_page_cost + params_.cpu_tuple_cost);
        return c;
    }

    Cost CostModel::index_scan(double rows_out, double table_rows, double correlation) const noexcept {
        // 有效随机读：物理相关高（|c|→1）时索引点查接近顺序读（PG effective_random_page_cost 思路）。
        const double corr = std::min(std::max(std::abs(correlation), 0.0), 1.0);
        const double effective_random =
                params_.random_page_cost * (1.0 - corr) + params_.seq_page_cost * corr;
        Cost c;
        c.startup = 0.0;
        const double rows = std::max(rows_out, 0.0);
        c.total = rows * (effective_random + params_.cpu_index_tuple_cost + params_.cpu_tuple_cost);
        // 空表防御：保证至少一次索引定位成本（table_rows>0 但选择率极低时仍优于 SeqScan）。
        (void)table_rows;
        return c;
    }

    // =========================================================================
    // 一元
    // =========================================================================

    Cost CostModel::filter(double child_total, double child_rows) const noexcept {
        Cost c;
        c.startup = 0.0; // 过滤流式输出，启动代价与子树合并看待
        c.total = child_total + std::max(child_rows, 0.0) * params_.cpu_operator_cost;
        return c;
    }

    Cost CostModel::sort(double child_total, double child_rows, double out_rows) const noexcept {
        const double n = std::max(child_rows, 2.0);
        const double cmp_cost = n * std::log2(n) * params_.cpu_operator_cost;
        Cost c;
        c.startup = child_total + cmp_cost;
        c.total = c.startup + std::max(out_rows, 0.0) * params_.cpu_tuple_cost;
        return c;
    }

    Cost CostModel::top_n_sort(double child_total, double child_rows, double k) const noexcept {
        const double heap_size = std::max(k, 2.0);
        const double heap_cost = heap_size * std::log2(heap_size) * params_.cpu_operator_cost;
        Cost c;
        c.startup = child_total + heap_cost;
        c.total = c.startup + heap_size * params_.cpu_tuple_cost;
        (void)child_rows;
        return c;
    }

    Cost CostModel::hash_aggregate(double child_total, double child_rows, double groups) const noexcept {
        Cost c;
        c.startup = child_total + std::max(child_rows, 0.0) * params_.cpu_operator_cost;
        c.total = c.startup + std::max(groups, 0.0) * params_.cpu_tuple_cost;
        return c;
    }

    Cost CostModel::sort_aggregate(double child_total, double child_rows) const noexcept {
        Cost c;
        c.startup = child_total;
        // 流式聚合：每输入行组比较 + 聚合更新（两次操作符代价）。
        c.total = child_total + std::max(child_rows, 0.0) * params_.cpu_operator_cost * 2.0;
        return c;
    }

    Cost CostModel::limit(double child_total) const noexcept {
        Cost c;
        c.startup = 0.0;
        c.total = child_total;
        return c;
    }

    Cost CostModel::union_(double arms_total) const noexcept {
        Cost c;
        c.startup = 0.0;
        c.total = arms_total;
        return c;
    }

    // =========================================================================
    // 连接
    // =========================================================================

    Cost CostModel::hash_join(double left_total, double right_total, double left_rows,
                              double right_rows, double out_rows) const noexcept {
        // 本引擎 HashJoin：右端全量建哈希（build），左端探测（probe）。
        Cost c;
        c.startup = right_total + std::max(right_rows, 0.0) * params_.cpu_operator_cost;
        c.total = c.startup + left_total +
                  std::max(left_rows, 0.0) * (params_.cpu_tuple_cost + params_.cpu_operator_cost) +
                  std::max(out_rows, 0.0) * params_.cpu_tuple_cost;
        return c;
    }

    Cost CostModel::merge_join(double left_total, double right_total, double left_rows,
                               double right_rows, double out_rows, bool need_sort_left,
                               bool need_sort_right) const noexcept {
        Cost l = sort(left_total, left_rows, left_rows);
        Cost r = sort(right_total, right_rows, right_rows);
        const double l_start = need_sort_left ? l.startup : left_total;
        const double r_start = need_sort_right ? r.startup : right_total;
        Cost c;
        c.startup = l_start + r_start;
        c.total = c.startup +
                  (std::max(left_rows, 0.0) + std::max(right_rows, 0.0)) * params_.cpu_operator_cost +
                  std::max(out_rows, 0.0) * params_.cpu_tuple_cost;
        return c;
    }

    Cost CostModel::nested_loop(double left_total, double right_total, double left_rows,
                                double out_rows) const noexcept {
        // 外层每行重执行内层（本引擎 NestedLoop 两侧物化，内层按外层行数放大）。
        Cost c;
        c.startup = left_total;
        const double lrows = std::max(left_rows, 1.0);
        c.total = left_total + lrows * right_total +
                  std::max(out_rows, 0.0) * params_.cpu_tuple_cost;
        return c;
    }

} // namespace corodb::opt
