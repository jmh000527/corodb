/**
 * @file bench_utils.cpp
 * @brief 并发压力测试辅助函数实现
 *
 * 本文件实现了压力测试的辅助功能：
 * - 延迟统计计算
 * - 格式化输出
 * - 报告生成
 *
 * @author CoroDB Team
 * @copyright Copyright (c) 2026 CoroDB Authors
 */

#include "bench_common.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <print>
#include <sstream>

namespace bench {

    using namespace std::chrono;
    using corodb::read_response;
    using corodb::send_line;

    // ============================================================================
    //                          网络辅助函数
    // ============================================================================

    std::string send_sql_and_recv(socket_t fd, const std::string& sql) {
        if (!send_line(fd, sql)) {
            return "";
        }
        return read_response(fd);
    }

    bool scenario_enabled(const std::string& scenario_name) {
        const auto& filter = g_bench_options.scenarios;
        if (filter.empty())
            return true;
        for (const auto& f: filter) {
            if (f.empty())
                continue;
            if (scenario_name.find(f) != std::string::npos)
                return true;
        }
        return false;
    }

    void apply_isolation(socket_t fd) {
        const auto& lvl = g_bench_options.isolation_level;
        if (lvl.empty())
            return;
        // 失败也忽略：服务器若不支持，不应影响 bench
        send_sql_and_recv(fd, "SET TRANSACTION ISOLATION LEVEL " + lvl);
    }

    // ============================================================================
    //                          统计计算函数
    // ============================================================================

    LatencyStats calculate_latency_stats(std::vector<double>& latencies) {
        LatencyStats stats{};
        if (latencies.empty()) {
            return stats;
        }

        // 排序以计算百分位数
        std::sort(latencies.begin(), latencies.end());

        // 计算平均值
        double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
        stats.avg_us = sum / static_cast<double>(latencies.size());

        // 计算百分位数
        auto percentile = [&](double p) -> double {
            size_t idx = static_cast<size_t>(p * static_cast<double>(latencies.size()));
            if (idx >= latencies.size())
                idx = latencies.size() - 1;
            return latencies[idx];
        };

        stats.p50_us = percentile(0.50);
        stats.p95_us = percentile(0.95);
        stats.p99_us = percentile(0.99);
        stats.min_us = latencies.front();
        stats.max_us = latencies.back();

        return stats;
    }

    // ============================================================================
    //                          格式化函数
    // ============================================================================

    std::string format_latency(double us) {
        if (us >= 1000000.0) {
            return std::format("{:.2f} s", us / 1000000.0);
        } else if (us >= 1000.0) {
            return std::format("{:.2f} ms", us / 1000.0);
        } else {
            return std::format("{:.2f} us", us);
        }
    }

    std::string get_timestamp() {
        auto now = system_clock::now();
        auto time_t_now = system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    // ============================================================================
    //                          报告生成函数
    // ============================================================================

    std::string generate_markdown_report(const TestReport& report) {
        std::ostringstream md;

        // 报告标题和测试环境
        md << "# CoroDB 并发性能测试报告\n\n";
        md << "## 测试环境\n\n";
        md << "| 项目 | 值 |\n";
        md << "|------|----|\n";
        md << "| 测试时间 | " << report.timestamp << " |\n";
        md << "| 并发客户端数 | " << report.concurrency << " |\n";
        md << "| 每客户端请求数 | " << report.requests_per_thread << " |\n";
        md << "| 总基准请求数 | " << report.concurrency * report.requests_per_thread << " |\n";
#ifdef _WIN32
        md << "| 操作系统 | Windows |\n";
#else
        md << "| 操作系统 | Linux/Unix |\n";
#endif
        md << "\n";

        // 各引擎详细结果
        for (const auto& engine: report.engine_results) {
            md << "## " << engine.engine_name << " 存储引擎\n\n";

            md << "| 测试场景 | 总请求 | 成功率 | Abort% | 隔离级别 | 耗时(s) | QPS | P50(ms) | P95(ms) | P99(ms) |\n";
            md << "|----------|--------|--------|--------|----------|---------|-----|---------|---------|---------|\n";

            for (const auto& r: engine.results) {
                double success_rate =
                        100.0 * static_cast<double>(r.success_requests) / static_cast<double>(r.total_requests);
                double abort_rate = (r.total_requests > 0) ? 100.0 * static_cast<double>(r.abort_count) /
                                                                     static_cast<double>(r.total_requests)
                                                           : 0.0;
                std::string iso = r.isolation_level.empty() ? "-" : r.isolation_level;
                md << "| " << r.test_name << " | " << r.total_requests << " | " << std::fixed << std::setprecision(1)
                   << success_rate << "%"
                   << " | " << std::setprecision(1) << abort_rate << "%"
                   << " | " << iso << " | " << std::setprecision(2) << r.duration_sec << " | " << std::setprecision(0)
                   << r.qps << " | " << std::setprecision(2) << r.p50_latency_ms << " | " << r.p95_latency_ms << " | "
                   << r.p99_latency_ms << " |\n";
            }
            md << "\n";
        }

        // 引擎对比总结表（多引擎时生成）
        if (report.engine_results.size() > 1) {
            md << "## 引擎性能对比\n\n";

            // 找到所有测试场景名称
            std::vector<std::string> test_names;
            if (!report.engine_results.empty() && !report.engine_results[0].results.empty()) {
                for (const auto& r: report.engine_results[0].results) {
                    test_names.push_back(r.test_name);
                }
            }

            // QPS 对比表
            md << "### QPS 对比\n\n";
            md << "| 测试场景 |";
            for (const auto& e: report.engine_results) {
                md << " " << e.engine_name << " |";
            }
            md << "\n|----------|";
            for (size_t i = 0; i < report.engine_results.size(); ++i) {
                md << "--------|";
            }
            md << "\n";

            for (size_t ti = 0; ti < test_names.size(); ++ti) {
                md << "| " << test_names[ti] << " |";
                for (const auto& e: report.engine_results) {
                    if (ti < e.results.size()) {
                        md << " " << std::fixed << std::setprecision(0) << e.results[ti].qps << " |";
                    } else {
                        md << " - |";
                    }
                }
                md << "\n";
            }
            md << "\n";

            // P99 延迟对比表
            md << "### P99 延迟对比 (ms)\n\n";
            md << "| 测试场景 |";
            for (const auto& e: report.engine_results) {
                md << " " << e.engine_name << " |";
            }
            md << "\n|----------|";
            for (size_t i = 0; i < report.engine_results.size(); ++i) {
                md << "--------|";
            }
            md << "\n";

            for (size_t ti = 0; ti < test_names.size(); ++ti) {
                md << "| " << test_names[ti] << " |";
                for (const auto& e: report.engine_results) {
                    if (ti < e.results.size()) {
                        md << " " << std::fixed << std::setprecision(2) << e.results[ti].p99_latency_ms << " |";
                    } else {
                        md << " - |";
                    }
                }
                md << "\n";
            }
            md << "\n";
        }

        md << "---\n";
        md << "*报告由 CoroDB Benchmark Tool 自动生成*\n";

        return md.str();
    }

    bool save_report(const std::string& report_content, const std::string& filename) {
        std::ofstream file(filename);
        if (!file) {
            std::println(std::cerr, "Failed to open file for writing: {}", filename);
            return false;
        }
        file << report_content;
        file.close();
        return true;
    }

} // namespace bench
