// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file cost_model.h @brief 统一代价模型（Phase 2，G1）：Cost{startup,total} 与各算子代价函数。 */

#pragma once

#include "corodb/common/config.h"

namespace corodb::opt {

    /** @brief 代价（PostgreSQL 语义）：startup = 产出首行前的代价，total = 全部输出代价。
     *
     * 比较语义与 PG 一致：先比 total，total 相同再比 startup（先出行的方案更利于流水线）。 */
    struct Cost {
        double startup{ 0.0 };
        double total{ 0.0 };

        /** @brief 代价比较：total 优先，其次 startup。 */
        [[nodiscard]] bool operator<(const Cost& o) const noexcept {
            if (total != o.total)
                return total < o.total;
            return startup < o.startup;
        }
        [[nodiscard]] bool operator<=(const Cost& o) const noexcept {
            return !(o < *this);
        }
    };

    /**
     * @brief 各物理算子的代价函数。
     *
     * 公式适配本引擎的执行特征（LSM 流式扫描，"行"同时充当"页"的计量单位），
     * 结构对齐 PostgreSQL cost_tuplestart/costseqscan 系列公式：
     *   - SeqScan：逐行顺序读 + CPU 元组处理；
     *   - IndexScan：超集索引逐 pk 点查 → 随机读计价，物理相关性（correlation）降低有效随机代价；
     *   - HashJoin：右端全量建哈希（startup），左端探测；
     *   - NLJoin：内侧重执行每外层行（本引擎两侧均物化，按外层行数放大内层代价）。
     * 默认参数对齐 PostgreSQL，全部经 [optimizer] 配置节可调。
     */
    class CostModel {
    public:
        /** @brief 代价参数集（默认值对齐 PostgreSQL）。 */
        struct Params {
            double seq_page_cost{ Config::kDefaultSeqPageCost };
            double random_page_cost{ Config::kDefaultRandomPageCost };
            double cpu_tuple_cost{ Config::kDefaultCpuTupleCost };
            double cpu_index_tuple_cost{ Config::kDefaultCpuIndexTupleCost };
            double cpu_operator_cost{ Config::kDefaultCpuOperatorCost };

            /** @brief 从全局 Config 读取参数。 */
            [[nodiscard]] static Params from_config();
        };

        CostModel() = default;
        explicit CostModel(Params p) noexcept : params_(p) {
        }

        // ---- 扫描 ----
        /** @brief 顺序扫描：rows × (seq_page_cost + cpu_tuple_cost)。 */
        [[nodiscard]] Cost seq_scan(double rows) const noexcept;

        /** @brief 索引扫描：输出行 × 有效随机读 + 索引项/元组 CPU。
         *
         * correlation ∈ [-1,1]（列值序与物理序的相关性）：|c|=1 时随机读退化为顺序读。 */
        [[nodiscard]] Cost index_scan(double rows_out, double table_rows, double correlation) const noexcept;

        // ---- 一元 ----
        /** @brief 过滤：子总代价 + 每输入行一次谓词求值。 */
        [[nodiscard]] Cost filter(double child_total, double child_rows) const noexcept;

        /** @brief 排序：n·log₂n 比较开销计入 startup，输出行计 CPU。 */
        [[nodiscard]] Cost sort(double child_total, double child_rows, double out_rows) const noexcept;

        /** @brief Top-N（K 元堆）：K·log K 代替 n·log n。 */
        [[nodiscard]] Cost top_n_sort(double child_total, double child_rows, double k) const noexcept;

        /** @brief 哈希聚合：每输入行一次哈希插入（startup），每组一次输出。 */
        [[nodiscard]] Cost hash_aggregate(double child_total, double child_rows, double groups) const noexcept;

        /** @brief 排序聚合（输入已按组键有序）：流式，每输入行常数代价。 */
        [[nodiscard]] Cost sort_aggregate(double child_total, double child_rows) const noexcept;

        /** @brief LIMIT：透传子代价（行数裁剪的收益由 Top-N 体现）。 */
        [[nodiscard]] Cost limit(double child_total) const noexcept;

        /** @brief UNION：各臂代价相加。 */
        [[nodiscard]] Cost union_(double arms_total) const noexcept;

        // ---- 连接 ----
        /** @brief 哈希连接：startup = 右端全量建表；total = 探测左端 + 输出。 */
        [[nodiscard]] Cost hash_join(double left_total, double right_total, double left_rows,
                                     double right_rows, double out_rows) const noexcept;

        /** @brief 归并连接：两端归并流式输出；需排序时把排序代价计入 startup。 */
        [[nodiscard]] Cost merge_join(double left_total, double right_total, double left_rows,
                                      double right_rows, double out_rows, bool need_sort_left,
                                      bool need_sort_right) const noexcept;

        /** @brief 嵌套循环：外层每行重执行内层（本引擎内层物化按此放大）。 */
        [[nodiscard]] Cost nested_loop(double left_total, double right_total, double left_rows,
                                       double out_rows) const noexcept;

        [[nodiscard]] const Params& params() const noexcept {
            return params_;
        }

    private:
        Params params_;
    };

} // namespace corodb::opt
