// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file metrics.h @brief 进程内指标注册表（Prometheus 文本格式暴露）。
 *
 * 生产观测基础设施（ROADMAP P2）：
 *   - Counter：单调递增计数（查询数、错误数、写入行数……）
 *   - Gauge：可升可降的瞬时值（活跃连接数、活跃事务数……）
 *   - Histogram：累积桶分布 + sum/count（查询延迟……）
 * 所有读写均为原子/加锁安全，可在任意线程采样。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corodb {

    /** @brief 单调递增计数器。 */
    class Counter {
    public:
        void increment(uint64_t v = 1) noexcept {
            value_.fetch_add(v, std::memory_order_relaxed);
        }
        [[nodiscard]] uint64_t value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<uint64_t> value_{ 0 };
    };

    /** @brief 瞬时值（有符号整数）。 */
    class Gauge {
    public:
        void set(int64_t v) noexcept {
            value_.store(v, std::memory_order_relaxed);
        }
        void increment(int64_t v = 1) noexcept {
            value_.fetch_add(v, std::memory_order_relaxed);
        }
        void decrement(int64_t v = 1) noexcept {
            value_.fetch_sub(v, std::memory_order_relaxed);
        }
        [[nodiscard]] int64_t value() const noexcept {
            return value_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<int64_t> value_{ 0 };
    };

    /** @brief 累积桶直方图（Prometheus 语义：bucket_i = ≤上界的累计样本数）。 */
    class Histogram {
    public:
        /** @brief 以显式上界序列构造（升序；+Inf 桶自动追加）。 */
        explicit Histogram(std::vector<double> upper_bounds);

        void observe(double v) noexcept;
        [[nodiscard]] const std::vector<double>& upper_bounds() const noexcept {
            return bounds_;
        }
        /** @brief 第 i 个桶的累计样本数（i == bounds.size() 为 +Inf 桶）。 */
        [[nodiscard]] uint64_t bucket_count(std::size_t i) const noexcept {
            return cumulative_[i].load(std::memory_order_relaxed);
        }
        [[nodiscard]] double sum() const noexcept {
            return sum_.load(std::memory_order_relaxed);
        }
        [[nodiscard]] uint64_t count() const noexcept {
            return count_.load(std::memory_order_relaxed);
        }

    private:
        std::vector<double> bounds_;
        std::unique_ptr<std::atomic<uint64_t>[]> cumulative_; ///< 累计桶（含 +Inf）
        std::size_t nbuckets_{ 0 };
        std::atomic<double> sum_{ 0.0 };
        std::atomic<uint64_t> count_{ 0 };
    };

    /**
     * @brief 进程级指标注册表（单例）。
     *
     * 指标按名惰性注册；名字与静态标签（如 {result="ok"}）在注册时确定。
     * render_prometheus() 输出 Prometheus 文本暴露格式（TYPE/名字 值）。
     */
    class Metrics {
    public:
        static Metrics& instance();

        /** @brief 取/建计数器；labels 为静态标签片段（可空，如 R"( {result="ok"}" )）。 */
        Counter& counter(const std::string& name, const std::string& labels = {},
                         const std::string& help = "");
        Gauge& gauge(const std::string& name, const std::string& labels = {},
                     const std::string& help = "");
        /** @brief 取/建直方图（注册时给桶界；重复注册沿用首次桶界）。 */
        Histogram& histogram(const std::string& name, const std::vector<double>& buckets,
                             const std::string& help = "");

        /** @brief 渲染 Prometheus 文本暴露格式（按名字排序，稳定输出）。 */
        [[nodiscard]] std::string render_prometheus() const;

        /** @brief 已注册指标数（测试用）。 */
        [[nodiscard]] std::size_t size() const;

    private:
        Metrics() = default;

        struct Entry {
            std::string name;
            std::string labels; // 形如 " {result=\"ok\"}"（带前导空格或空）
            std::string help;
            enum class Kind { Counter, Gauge, Histogram } kind{ Kind::Counter };
            std::unique_ptr<Counter> counter;
            std::unique_ptr<Gauge> gauge;
            std::unique_ptr<Histogram> histogram;
        };

        mutable std::mutex mutex_;
        std::map<std::string, Entry> metrics_; // 按 (名+标签) 排序，渲染稳定
    };

} // namespace corodb
