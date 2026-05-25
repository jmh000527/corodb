/**
 * @file bench_common.h
 * @brief 并发压力测试公共定义
 *
 * 本文件包含压力测试的公共数据结构、常量和函数声明。
 *
 * @author CoroDB Team
 * @copyright Copyright (c) 2026 CoroDB Authors
 */

#pragma once

#include "corodb/net/network.h"
#include "corodb/net/port.h"
#include "corodb/server/server.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
//                          类型别名
// ============================================================================

using Clock = std::chrono::high_resolution_clock;

/// SQL生成器类型：接收请求ID，返回SQL语句
using SqlGenerator = std::function<std::string(int)>;

// ============================================================================
//                          全局配置常量
// ============================================================================

namespace bench {

    constexpr int kServerPort = 14123;             ///< 测试服务器端口（避开本地常用端口 14000）
    constexpr int kDefaultConcurrency = 50;         ///< 默认并发数
    constexpr int kDefaultRequestsPerThread = 1500; ///< 每线程默认请求数

    // ============================================================================
    //                          全局状态（用于线程间通信）
    //
    // T6.1：所有共享状态从“裸 inline 全局”迁移到 BenchContext 中集中管理。
    // 现有调用点通过下方 `g_xxx` 引用别名访问 BenchContext 成员，保持源码兼容；
    // 单元测试或未来需要隔离运行多个 bench 实例时，可直接构造独立 BenchContext。
    // ============================================================================

    /**
     * @brief Bench 全局可配置选项（T6.5）
     *
     * 通过命令行参数填充，由 run_engine_tests 与各场景代码读取。
     */
    struct BenchOptions {
        std::vector<std::string> scenarios; ///< 仅运行匹配名称（substring）的场景；空 = 全部
        int repeat{ 1 };                    ///< 重复轮数（取最佳 QPS 那次为最终结果）
        int warmup{ 0 };                    ///< 每场景预热请求数（0 = 不预热）
        unsigned seed{ 0 };                 ///< RNG 种子（0 = 用 std::time）
        std::string isolation_level;        ///< 在每个连接初始化时执行的 SET TRANSACTION ISOLATION LEVEL（空 = 默认）
    };

    /**
     * @brief Bench 共享上下文（T6.1）
     *
     * 集中持有所有 bench 期间被多线程读写的可变状态：
     *   - 请求/失败计数原子量
     *   - 延迟样本数组及其互斥锁
     *   - 服务器就绪标志
     *   - BenchOptions 配置
     *
     * 使用 reset_counters() 在每轮场景之间清零，避免不同场景的统计相互污染。
     * 单元测试可以构造独立 BenchContext 实例做隔离测试。
     */
    struct BenchContext {
        std::atomic<bool> server_ready{ false };
        std::atomic<int64_t> total_requests{ 0 };
        std::atomic<int64_t> successful_requests{ 0 };
        std::atomic<int64_t> failed_requests{ 0 };

        std::mutex latency_mutex;
        std::vector<double> latencies_us;

        BenchOptions options{};

        /// 在每轮场景开始前清空计数与延迟样本（保留 server_ready 与 options）。
        void reset_counters() {
            total_requests.store(0);
            successful_requests.store(0);
            failed_requests.store(0);
            std::lock_guard<std::mutex> lk(latency_mutex);
            latencies_us.clear();
        }
    };

    /// 进程级单例 BenchContext。函数局部 static 保证首次调用时构造，
    /// 析构顺序符合 C++ 标准的反向构造序列。
    inline BenchContext& bench_ctx() {
        static BenchContext ctx;
        return ctx;
    }

    /// 兼容别名：维持原有 `g_xxx` 调用点，避免大面积改动。
    /// 这些是引用别名（C++17 inline 变量 + 引用初始化），共享同一份 BenchContext 存储。
    inline std::atomic<bool>& g_server_ready = bench_ctx().server_ready;
    inline std::atomic<int64_t>& g_total_requests = bench_ctx().total_requests;
    inline std::atomic<int64_t>& g_successful_requests = bench_ctx().successful_requests;
    inline std::atomic<int64_t>& g_failed_requests = bench_ctx().failed_requests;
    inline std::mutex& g_latency_mutex = bench_ctx().latency_mutex;
    inline std::vector<double>& g_latencies_us = bench_ctx().latencies_us;
    inline BenchOptions& g_bench_options = bench_ctx().options;

    // ============================================================================
    //                          数据结构定义
    // ============================================================================

    /**
     * @brief 延迟统计信息
     */
    struct LatencyStats {
        double avg_us{ 0 }; ///< 平均延迟（微秒）
        double p50_us{ 0 }; ///< P50延迟
        double p95_us{ 0 }; ///< P95延迟
        double p99_us{ 0 }; ///< P99延迟
        double min_us{ 0 }; ///< 最小延迟
        double max_us{ 0 }; ///< 最大延迟
    };

    /**
     * @brief 单个测试项的结果
     */
    struct BenchmarkResult {
        std::string test_name;         ///< 测试名称
        int concurrency{ 0 };          ///< 并发数
        int64_t total_requests{ 0 };   ///< 总请求数
        int64_t success_requests{ 0 }; ///< 成功请求数
        int64_t failed_requests{ 0 };  ///< 失败请求数
        int64_t abort_count{ 0 };      ///< 事务 abort 数（仅事务/隔离场景有意义）
        std::string isolation_level;   ///< 该场景使用的隔离级别（默认空 = 服务端默认）
        double duration_sec{ 0 };      ///< 持续时间（秒）
        double qps{ 0 };               ///< 每秒查询数
        double avg_latency_ms{ 0 };    ///< 平均延迟（毫秒）
        double p50_latency_ms{ 0 };    ///< P50延迟
        double p95_latency_ms{ 0 };    ///< P95延迟
        double p99_latency_ms{ 0 };    ///< P99延迟
        double min_latency_ms{ 0 };    ///< 最小延迟
        double max_latency_ms{ 0 };    ///< 最大延迟
    };

    /**
     * @brief 单个引擎的测试结果集合
     */
    struct EngineTestResults {
        std::string engine_name;              ///< 引擎名称
        std::vector<BenchmarkResult> results; ///< 所有测试结果
    };

    /**
     * @brief 完整的测试报告
     */
    struct TestReport {
        std::string timestamp;                         ///< 测试时间戳
        int concurrency{ 0 };                          ///< 并发数
        int requests_per_thread{ 0 };                  ///< 每线程请求数
        std::vector<EngineTestResults> engine_results; ///< 各引擎测试结果
    };

    // BenchOptions 与 g_bench_options 已迁移到 BenchContext（见文件顶部）。

    /**
     * @brief 判断场景名是否需要运行（T6.5 --scenario 过滤）
     */
    bool scenario_enabled(const std::string& scenario_name);

    /**
     * @brief 在已建立的连接上设置隔离级别（如配置）
     */
    void apply_isolation(socket_t fd);

    // ============================================================================
    //                          辅助函数声明
    // ============================================================================

    /**
     * @brief 发送SQL并接收响应
     * @param fd Socket文件描述符
     * @param sql SQL语句
     * @return 响应字符串，失败返回空串
     */
    std::string send_sql_and_recv(socket_t fd, const std::string& sql);

    /**
     * @brief 计算延迟统计信息
     * @param latencies 延迟数据（微秒），会被排序
     * @return 延迟统计结果
     */
    LatencyStats calculate_latency_stats(std::vector<double>& latencies);

    /**
     * @brief 格式化延迟值为可读字符串
     * @param us 延迟（微秒）
     * @return 格式化字符串（自动选择单位：us/ms/s）
     */
    std::string format_latency(double us);

    /**
     * @brief 获取当前时间戳字符串
     * @return 格式化的时间戳 (YYYY-MM-DD HH:MM:SS)
     */
    std::string get_timestamp();

    /**
     * @brief 生成Markdown格式的测试报告
     * @param report 测试报告数据
     * @return Markdown格式的字符串
     */
    std::string generate_markdown_report(const TestReport& report);

    /**
     * @brief 保存报告到文件
     * @param report_content 报告内容
     * @param filename 文件名
     * @return 是否保存成功
     */
    bool save_report(const std::string& report_content, const std::string& filename);

    // ============================================================================
    //                          测试执行函数声明
    // ============================================================================

    /**
     * @brief 单线程压测工作函数
     * @param thread_id 线程ID
     * @param requests_per_thread 每线程请求数
     * @param sql_generator SQL生成函数
     */
    void client_worker(int thread_id, int requests_per_thread, const SqlGenerator& sql_generator);

    /**
     * @brief 初始化测试数据
     * @param fd 连接描述符
     */
    void setup_test_data(socket_t fd);

    /**
     * @brief 运行并发测试
     * @param test_name 测试名称
     * @param concurrency 并发数
     * @param requests_per_thread 每线程请求数
     * @param sql_generator SQL生成器
     * @return BenchmarkResult 测试结果
     */
    BenchmarkResult run_benchmark(const std::string& test_name, int concurrency, int requests_per_thread,
                                  const SqlGenerator& sql_generator);

    /**
     * @brief 运行单个引擎的所有测试场景
     * @param engine 引擎类型
     * @param engine_name 引擎名称
     * @param concurrency 并发数
     * @param requests_per_thread 每线程请求数
     * @return EngineTestResults 该引擎的所有测试结果
     */
    EngineTestResults run_engine_tests(const std::string& engine_name, int concurrency,
                                       int requests_per_thread);

} // namespace bench
