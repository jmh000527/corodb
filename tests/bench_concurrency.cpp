/**
 * @file bench_concurrency.cpp
 * @brief 协程服务器并发压力测试
 *
 * 本文件实现了CoroDB协程服务器的并发性能测试，包括：
 * - 多线程并发客户端模拟
 * - 吞吐量（QPS）测量
 * - 延迟统计（平均、P50、P95、P99）
 * - 连接建立/释放压力测试
 * - 自动生成测试报告（Markdown格式）
 *
 * 测试场景：
 * 1. 单表查询并发测试
 * 2. 多表JOIN并发测试
 * 3. INSERT并发测试
 * 4. 混合读写并发测试
 *
 * 使用方式：
 * - 一键运行：直接执行，自动测试 LSM 引擎并生成报告
 * - 自定义参数：./bench_concurrency -c 50 -n 200
 *
 * @author CoroDB Team
 * @copyright Copyright (c) 2026 CoroDB Authors
 */

#include "bench_common.h"

#include "corodb/common/config.h"
#include "corodb/storage/storage_engine_common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <thread>
#include <vector>

using namespace std::chrono;
using namespace bench;

// ============================================================================
//                          客户端压测线程
// ============================================================================

namespace bench {

    using corodb::connect_socket;
    using corodb::is_error_response;
    using corodb::wait_for_server;

    void client_worker(int thread_id, int requests_per_thread, const SqlGenerator& sql_generator) {
        // 等待服务器真正在 listen（替代 g_server_ready 竞态：B4）
        if (!wait_for_server("127.0.0.1", kServerPort, 5000)) {
            std::println(std::cerr, "[Thread-{}] Server not ready in time", thread_id);
            return;
        }

        // 连接到服务器（阻塞 socket：客户端无需 nonblock；B5）
        socket_t fd = connect_socket("127.0.0.1", kServerPort);
        if (fd == INVALID_SOCKET_VAL) {
            std::println(std::cerr, "[Thread-{}] Failed to connect to server", thread_id);
            return;
        }
        apply_isolation(fd); // T6.5 --isolation

        std::vector<double> local_latencies;
        local_latencies.reserve(static_cast<size_t>(requests_per_thread));

        // 执行请求
        for (int i = 0; i < requests_per_thread; ++i) {
            std::string sql = sql_generator(thread_id * requests_per_thread + i);

            auto start = Clock::now();
            std::string response = send_sql_and_recv(fd, sql);
            auto end = Clock::now();

            double latency_us = static_cast<double>(duration_cast<microseconds>(end - start).count());
            local_latencies.push_back(latency_us);

            g_total_requests.fetch_add(1);
            // 修复 B2：服务器返回 "ERROR:"（大写），原 find("Error") 永远不命中
            if (!response.empty() && !is_error_response(response)) {
                g_successful_requests.fetch_add(1);
            } else {
                g_failed_requests.fetch_add(1);
                // 打印失败详情（仅前几个）
                if (g_failed_requests.load() <= 5) {
                    std::println(std::cerr, "[Thread-{}] Failed: SQL='{}', Response='{}'", thread_id, sql.substr(0, 60),
                                 response.empty() ? "(empty)" : response.substr(0, 100));
                }
            }
        }

        close_socket(fd);

        // 合并延迟数据
        {
            std::lock_guard<std::mutex> lock(g_latency_mutex);
            g_latencies_us.insert(g_latencies_us.end(), local_latencies.begin(), local_latencies.end());
        }
    }

    // ============================================================================
    //                          数据基线管理
    // ============================================================================

    namespace {
        // 基线参数：用户和订单的 user_id 范围对齐，避免 bench_orders 引用不存在的用户
        constexpr int kBatchSize = 100;
        constexpr int kTotalUsers = 1500;
        constexpr int kTotalOrders = 2500;
        constexpr int kTotalProducts = 1000;

        /**
         * @brief 检查响应是否成功（非空 + 非 ERROR:）
         */
        bool ok(const std::string& resp) {
            return !resp.empty() && !is_error_response(resp);
        }

        /**
         * @brief 在事务中批量插入数据
         * @return 实际成功插入的行数
         */
        int seed_baseline(socket_t fd) {
            int total_rows = 0;

            if (!ok(send_sql_and_recv(fd, "BEGIN"))) {
                std::println(std::cerr, "  [seed] BEGIN failed");
                return 0;
            }

            // 用户
            for (int s = 1; s <= kTotalUsers; s += kBatchSize) {
                std::string sql = "INSERT INTO bench_users VALUES ";
                int e = std::min(s + kBatchSize - 1, kTotalUsers);
                for (int i = s; i <= e; ++i) {
                    if (i > s)
                        sql += ", ";
                    sql += std::format("({}, 'User{}', {})", i, i, 20 + (i % 50));
                }
                if (ok(send_sql_and_recv(fd, sql)))
                    total_rows += (e - s + 1);
            }

            // 订单：user_id 严格落在 [1, kTotalUsers] 范围内
            for (int s = 1; s <= kTotalOrders; s += kBatchSize) {
                std::string sql = "INSERT INTO bench_orders VALUES ";
                int e = std::min(s + kBatchSize - 1, kTotalOrders);
                for (int i = s; i <= e; ++i) {
                    int user_id = ((i - 1) % kTotalUsers) + 1;
                    if (i > s)
                        sql += ", ";
                    sql += std::format("({}, {}, {})", i, user_id, 100 + (i * 5));
                }
                if (ok(send_sql_and_recv(fd, sql)))
                    total_rows += (e - s + 1);
            }

            // 产品
            for (int s = 1; s <= kTotalProducts; s += kBatchSize) {
                std::string sql = "INSERT INTO bench_products VALUES ";
                int e = std::min(s + kBatchSize - 1, kTotalProducts);
                for (int i = s; i <= e; ++i) {
                    if (i > s)
                        sql += ", ";
                    sql += std::format("({}, 'Product{}', {})", i, i, 50 + (i % 200));
                }
                if (ok(send_sql_and_recv(fd, sql)))
                    total_rows += (e - s + 1);
            }

            if (!ok(send_sql_and_recv(fd, "COMMIT"))) {
                std::println(std::cerr, "  [seed] COMMIT failed");
                send_sql_and_recv(fd, "ROLLBACK");
                return 0;
            }
            return total_rows;
        }

        /**
         * @brief 重置（DROP+CREATE+seed）所有 bench 表
         *
         * 在每个写场景前调用，保证场景之间互不污染（用户要求）。
         */
        bool reset_baseline(socket_t fd) {
            const char* drop_sqls[] = {
                "DROP TABLE IF EXISTS bench_users",
                "DROP TABLE IF EXISTS bench_orders",
                "DROP TABLE IF EXISTS bench_products",
            };
            for (auto* s: drop_sqls) {
                auto r = send_sql_and_recv(fd, s);
                if (!ok(r)) {
                    std::println(std::cerr, "  [reset] {} failed: {}", s, r.substr(0, 80));
                    return false;
                }
            }
            const char* create_sqls[] = {
                "CREATE TABLE bench_users (id INT, name TEXT, age INT)",
                "CREATE TABLE bench_orders (id INT, user_id INT, amount INT)",
                "CREATE TABLE bench_products (id INT, name TEXT, price INT)",
            };
            for (auto* s: create_sqls) {
                auto r = send_sql_and_recv(fd, s);
                if (!ok(r)) {
                    std::println(std::cerr, "  [reset] {} failed: {}", s, r.substr(0, 80));
                    return false;
                }
            }
            int rows = seed_baseline(fd);
            return rows > 0;
        }
    } // anonymous namespace

    // ============================================================================
    //                          测试数据初始化
    // ============================================================================

    /**
     * @brief 首次建立基线（创建表 + 灌数据）
     */
    void setup_test_data(socket_t fd) {
        auto start_time = Clock::now();
        if (!reset_baseline(fd)) {
            std::println(std::cerr, "  [ERROR] baseline setup failed");
            return;
        }
        auto ms = duration_cast<milliseconds>(Clock::now() - start_time).count();
        std::println("  Baseline ready: {} rows  ({} ms)", kTotalUsers + kTotalOrders + kTotalProducts, ms);
    }

    // ============================================================================
    //                          基准测试执行
    // ============================================================================

    BenchmarkResult run_benchmark(const std::string& test_name, int concurrency, int requests_per_thread,
                                  const SqlGenerator& sql_generator) {
        // Print scenario name first (no newline) so user sees it while running
        std::print("  {:<32}", test_name);
        std::fflush(stdout);

        // 重置统计（T6.1 BenchContext 集中管理）
        bench_ctx().reset_counters();
        {
            std::lock_guard<std::mutex> lock(g_latency_mutex);
            g_latencies_us.reserve(static_cast<size_t>(concurrency * requests_per_thread));
        }

        // 启动计时
        auto start_time = Clock::now();

        // 启动客户端线程
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(concurrency));
        for (int i = 0; i < concurrency; ++i) {
            threads.emplace_back(client_worker, i, requests_per_thread, sql_generator);
        }

        // 等待所有线程完成
        for (auto& t: threads) {
            t.join();
        }

        auto end_time = Clock::now();
        double duration_sec = static_cast<double>(duration_cast<milliseconds>(end_time - start_time).count()) / 1000.0;

        // 计算统计数据
        int64_t total = g_total_requests.load();
        int64_t success = g_successful_requests.load();
        int64_t failed = g_failed_requests.load();
        double qps = static_cast<double>(total) / duration_sec;
        double success_pct = 100.0 * static_cast<double>(success) / static_cast<double>(total);

        LatencyStats stats;
        {
            std::lock_guard<std::mutex> lock(g_latency_mutex);
            stats = calculate_latency_stats(g_latencies_us);
        }

        // 输出结果（紧接在 test_name 后同行）
        std::println("  {:5.2f}s  {:>5} req  {:>6.0f} QPS  {:>5.1f}%  P50:{:<9} P99:{}",
                     duration_sec, total, qps, success_pct,
                     format_latency(stats.p50_us), format_latency(stats.p99_us));

        // 构建并返回结果
        BenchmarkResult result;
        result.test_name = test_name;
        result.concurrency = concurrency;
        result.total_requests = total;
        result.success_requests = success;
        result.failed_requests = failed;
        result.duration_sec = duration_sec;
        result.qps = qps;
        result.avg_latency_ms = stats.avg_us / 1000.0;
        result.p50_latency_ms = stats.p50_us / 1000.0;
        result.p95_latency_ms = stats.p95_us / 1000.0;
        result.p99_latency_ms = stats.p99_us / 1000.0;
        result.min_latency_ms = stats.min_us / 1000.0;
        result.max_latency_ms = stats.max_us / 1000.0;
        return result;
    }

    // ============================================================================
    //                          引擎测试封装
    // ============================================================================

    EngineTestResults run_engine_tests(const std::string& engine_name, int concurrency,
                                       int requests_per_thread) {
        EngineTestResults engine_results;
        engine_results.engine_name = engine_name;

        // 临时数据目录：时间戳 + PID + 随机数，避免并发跑测时冲突（D5）
        std::string data_dir = std::format("bench_data_{}_{}_{}_{}", engine_name, std::time(nullptr),
                                           static_cast<long>(
#ifdef _WIN32
                                                   ::GetCurrentProcessId()
#else
                                                   ::getpid()
#endif
                                                           ),
                                           std::rand());
        std::filesystem::create_directories(data_dir);

        // 启动服务器线程（诊断消息已重定向到 stderr）
        std::thread server_thread([&data_dir]() {
            try {
                corodb::ServerConfig config;
                config.port = kServerPort;
                config.data_dir = data_dir;
                corodb::run_server(config);
            } catch (const std::exception& e) {
                std::println(std::cerr, "[Server] Error: {}", e.what());
            }
        });

        // 等待服务器真正监听端口（修复 B4：原 g_server_ready 在 listen 之前就被置位）
        if (!wait_for_server("127.0.0.1", kServerPort, 5000)) {
            std::println(std::cerr, "Server failed to start on port {}", kServerPort);
            corodb::request_server_shutdown();
            server_thread.join();
            std::filesystem::remove_all(data_dir);
            return engine_results;
        }

        // 连接并初始化测试数据（保持 setup_fd 存活以便后续 rebaseline）
        socket_t setup_fd = connect_socket("127.0.0.1", kServerPort);
        if (setup_fd == INVALID_SOCKET_VAL) {
            std::println(std::cerr, "Failed to connect for setup");
            corodb::request_server_shutdown();
            server_thread.join();
            std::filesystem::remove_all(data_dir);
            return engine_results;
        }
        setup_test_data(setup_fd);

        // 每个写场景前重建基线，避免场景互相污染（用户要求）
        auto rebaseline = [&]() {
            if (!reset_baseline(setup_fd)) {
                std::println(std::cerr, "  [WARN] rebaseline failed, results may be inaccurate");
            } else {
                std::println("  {:-<60}", "─── reset baseline  ");
            }
        };

        // T6.5: --scenario 过滤的统一 wrapper。
        auto run_if = [&](const std::string& name, int conc, int req, SqlGenerator gen) {
            if (!scenario_enabled(name)) {
                std::println("  {:<32}  (skipped)", name);
                return;
            }
            auto r = run_benchmark(name, conc, req, std::move(gen));
            r.isolation_level = g_bench_options.isolation_level;
            engine_results.results.push_back(r);
        };

        // 测试1：简单SELECT查询（点查 + 范围查询）
        run_if("Simple SELECT", concurrency, requests_per_thread, [](int req_id) -> std::string {
            // 混合：50% 点查，30% 范围查询，20% IN 查询
            int type = req_id % 10;
            if (type < 5) {
                // 点查
                return std::format("SELECT id, name, age FROM bench_users WHERE id = {}", (req_id % 1000) + 1);
            } else if (type < 8) {
                // 范围查询 (BETWEEN)
                int start = (req_id % 500) + 1;
                return std::format("SELECT id, name FROM bench_users WHERE age BETWEEN {} AND {}", 20 + (req_id % 30),
                                   40 + (req_id % 20));
            } else {
                // IN 查询
                int id1 = (req_id % 100) + 1;
                int id2 = (req_id % 100) + 101;
                int id3 = (req_id % 100) + 201;
                return std::format("SELECT id, name FROM bench_users WHERE id IN ({}, {}, {})", id1, id2, id3);
            }
        });

        // 测试2：全表扫描 + 过滤条件
        run_if("Full Table Scan", concurrency, requests_per_thread / 2, [](int req_id) -> std::string {
            // 混合：简单扫描、LIKE 模式匹配、多条件过滤
            int type = req_id % 4;
            if (type == 0) {
                return "SELECT id, name, age FROM bench_users";
            } else if (type == 1) {
                return std::format("SELECT id, name FROM bench_users WHERE name LIKE 'User{}%'", req_id % 10);
            } else if (type == 2) {
                return std::format("SELECT DISTINCT age FROM bench_users WHERE age > {}", 20 + (req_id % 40));
            } else {
                // 多条件 AND/OR
                return std::format("SELECT id, name FROM bench_users WHERE age > {} AND age < {} OR id < 100",
                                   20 + (req_id % 20), 50 + (req_id % 20));
            }
        });

        // 测试3：聚合查询 (COUNT/SUM/AVG/MIN/MAX + GROUP BY)
        run_if("Aggregate Query (COUNT/SUM)", concurrency, requests_per_thread, [](int req_id) -> std::string {
            int type = req_id % 6;
            switch (type) {
                case 0:
                    return "SELECT count(*) FROM bench_orders";
                case 1:
                    return "SELECT sum(amount) FROM bench_orders";
                case 2:
                    return "SELECT avg(amount) FROM bench_orders";
                case 3:
                    return "SELECT min(amount), max(amount) FROM bench_orders";
                case 4:
                    return "SELECT user_id, count(*) FROM bench_orders GROUP BY user_id LIMIT 50";
                default:
                    return "SELECT user_id, sum(amount) FROM bench_orders GROUP BY user_id HAVING sum(amount) > 1000 "
                           "LIMIT 20";
            }
        });

        // 测试4：JOIN查询（使用表限定符的版本）
        run_if("JOIN Query", std::max(1, concurrency / 2), requests_per_thread / 2, [](int req_id) -> std::string {
            int type = req_id % 4;
            if (type == 0) {
                // INNER JOIN - 简单点查
                return std::format(
                        "SELECT bench_users.name, bench_orders.amount FROM bench_users "
                        "JOIN bench_orders ON bench_users.id = bench_orders.user_id WHERE bench_users.id = {}",
                        (req_id % 500) + 1);
            } else if (type == 1) {
                // LEFT JOIN - 范围查询
                return std::format("SELECT bench_users.name, bench_orders.amount FROM bench_users "
                                   "LEFT JOIN bench_orders ON bench_users.id = bench_orders.user_id WHERE "
                                   "bench_users.id BETWEEN {} AND {}",
                                   (req_id % 100) + 1, (req_id % 100) + 10);
            } else if (type == 2) {
                // JOIN + 聚合
                return std::format("SELECT bench_users.name, count(*) FROM bench_users "
                                   "JOIN bench_orders ON bench_users.id = bench_orders.user_id WHERE bench_users.age > "
                                   "{} GROUP BY bench_users.name LIMIT 20",
                                   25 + (req_id % 30));
            } else {
                // 简单产品查询
                return std::format("SELECT name, price FROM bench_products WHERE id = {}", (req_id % 200) + 1);
            }
        });

        // -------- 写场景：每场景前重建基线 --------

        // 测试5：Concurrent INSERT（独立 id 区间）
        if (scenario_enabled("Concurrent INSERT"))
            rebaseline();
        run_if("Concurrent INSERT", concurrency, requests_per_thread / 4, [](int req_id) -> std::string {
            if (req_id % 3 == 0) {
                int base_id = 100000 + req_id * 3;
                return std::format("INSERT INTO bench_users VALUES ({}, 'BatchUser{}', {}), ({}, 'BatchUser{}', {}), "
                                   "({}, 'BatchUser{}', {})",
                                   base_id, base_id, 20 + (req_id % 40), base_id + 1, base_id + 1, 21 + (req_id % 40),
                                   base_id + 2, base_id + 2, 22 + (req_id % 40));
            } else {
                return std::format("INSERT INTO bench_users VALUES ({}, 'NewUser{}', {})", 50000 + req_id, req_id,
                                   25 + (req_id % 45));
            }
        });

        // 测试6：Concurrent UPDATE（基于 baseline 范围内的真实行）
        if (scenario_enabled("Concurrent UPDATE"))
            rebaseline();
        run_if("Concurrent UPDATE", concurrency, requests_per_thread / 4, [](int req_id) -> std::string {
            int target = (req_id % kTotalUsers) + 1;
            if (req_id % 2 == 0) {
                return std::format("UPDATE bench_users SET age = {} WHERE id = {}", 20 + (req_id % 60), target);
            } else {
                return std::format("UPDATE bench_orders SET amount = amount + {} WHERE user_id = {}", (req_id % 10) + 1,
                                   target);
            }
        });

        // 测试7：Concurrent DELETE
        if (scenario_enabled("Concurrent DELETE"))
            rebaseline();
        run_if("Concurrent DELETE", concurrency, requests_per_thread / 4, [](int req_id) -> std::string {
            int id = 1 + req_id;
            if (id <= kTotalOrders) {
                return std::format("DELETE FROM bench_orders WHERE id = {}", id);
            }
            // 越界后转向产品表（避免动到只读 baseline 之外的逻辑空表）
            int pid = 1 + (req_id % kTotalProducts);
            return std::format("DELETE FROM bench_products WHERE id = {}", pid);
        });

        // 测试8：混合读写（80% Read, 20% Write，使用真实的 UPDATE/DELETE）
        if (scenario_enabled("Mixed Read/Write (80R/20W)"))
            rebaseline();
        run_if("Mixed Read/Write (80R/20W)", concurrency, requests_per_thread, [](int req_id) -> std::string {
            int type = req_id % 10;
            if (type == 0) {
                return std::format("INSERT INTO bench_orders VALUES ({}, {}, {})", 200000 + req_id,
                                   ((req_id - 1 + kTotalUsers) % kTotalUsers) + 1, 100 + (req_id % 900));
            } else if (type == 1) {
                return std::format("UPDATE bench_users SET age = {} WHERE id = {}", 20 + (req_id % 60),
                                   (req_id % kTotalUsers) + 1);
            } else {
                int read_type = type % 4;
                switch (read_type) {
                    case 0:
                        return std::format("SELECT * FROM bench_users WHERE id = {}", (req_id % kTotalUsers) + 1);
                    case 1:
                        return std::format(
                                "SELECT id, user_id, amount FROM bench_orders WHERE amount BETWEEN {} AND {}",
                                100 + (req_id % 400), 500 + (req_id % 500));
                    case 2:
                        return std::format("SELECT count(*) FROM bench_orders WHERE user_id = {}",
                                           (req_id % kTotalUsers) + 1);
                    default:
                        return std::format(
                                "SELECT bench_users.name, bench_orders.amount FROM bench_users "
                                "JOIN bench_orders ON bench_users.id = bench_orders.user_id WHERE bench_orders.id = {}",
                                (req_id % kTotalOrders) + 1);
                }
            }
        });

        // -------- T6.4 Transaction Throughput: BEGIN; n DML; COMMIT --------
        // 重建基线后跑：每个事务包含 5 个 UPDATE，目标行按 thread_id 分桶避免冲突。
        // 每事务作为一次"逻辑请求"，QPS 即为 transaction-per-second。
        if (scenario_enabled("Transaction Throughput")) {
            rebaseline();
            const std::string scen = "Transaction Throughput (5 DML)";
            std::print("  {:<32}", scen);
            std::fflush(stdout);
            bench_ctx().reset_counters();

            const int txns_per_thread = std::max(1, requests_per_thread / 16);
            auto start_time = Clock::now();
            std::vector<std::thread> threads;
            for (int tid = 0; tid < concurrency; ++tid) {
                threads.emplace_back([tid, txns_per_thread]() {
                    if (!wait_for_server("127.0.0.1", kServerPort, 5000))
                        return;
                    socket_t fd = connect_socket("127.0.0.1", kServerPort);
                    if (fd == INVALID_SOCKET_VAL)
                        return;
                    apply_isolation(fd);
                    std::vector<double> local;
                    local.reserve(static_cast<size_t>(txns_per_thread));
                    // 按 thread_id 划分写域：每线程独占 100 个 user id，避免天然冲突
                    int base = 1 + tid * 100;
                    for (int i = 0; i < txns_per_thread; ++i) {
                        auto t0 = Clock::now();
                        bool failed = false;
                        if (!ok(send_sql_and_recv(fd, "BEGIN")))
                            failed = true;
                        for (int k = 0; k < 5 && !failed; ++k) {
                            int target = base + ((i + k) % 100);
                            std::string sql = std::format("UPDATE bench_users SET age = {} WHERE id = {}",
                                                          20 + ((i * 7 + k) % 60), target);
                            if (!ok(send_sql_and_recv(fd, sql))) {
                                failed = true;
                                break;
                            }
                        }
                        if (failed) {
                            send_sql_and_recv(fd, "ROLLBACK");
                        } else if (!ok(send_sql_and_recv(fd, "COMMIT"))) {
                            failed = true;
                        }
                        auto t1 = Clock::now();
                        local.push_back(static_cast<double>(duration_cast<microseconds>(t1 - t0).count()));
                        g_total_requests.fetch_add(1);
                        if (failed)
                            g_failed_requests.fetch_add(1);
                        else
                            g_successful_requests.fetch_add(1);
                    }
                    close_socket(fd);
                    std::lock_guard<std::mutex> lk(g_latency_mutex);
                    g_latencies_us.insert(g_latencies_us.end(), local.begin(), local.end());
                });
            }
            for (auto& t: threads)
                t.join();
            auto end_time = Clock::now();
            double sec = static_cast<double>(duration_cast<milliseconds>(end_time - start_time).count()) / 1000.0;
            int64_t total = g_total_requests.load();
            int64_t success = g_successful_requests.load();
            double tps = static_cast<double>(total) / sec;
            LatencyStats st;
            {
                std::lock_guard<std::mutex> lk(g_latency_mutex);
                st = calculate_latency_stats(g_latencies_us);
            }
            std::println("  {:5.2f}s  {:>5} txn  {:>6.0f} TPS  {:>5.1f}%  P50:{:<9} P99:{}", sec, total, tps,
                         100.0 * static_cast<double>(success) / static_cast<double>(std::max<int64_t>(1, total)),
                         format_latency(st.p50_us), format_latency(st.p99_us));
            BenchmarkResult r;
            r.test_name = scen;
            r.concurrency = concurrency;
            r.total_requests = total;
            r.success_requests = success;
            r.failed_requests = total - success;
            r.abort_count = total - success;
            r.isolation_level = g_bench_options.isolation_level;
            r.duration_sec = sec;
            r.qps = tps;
            r.avg_latency_ms = st.avg_us / 1000.0;
            r.p50_latency_ms = st.p50_us / 1000.0;
            r.p95_latency_ms = st.p95_us / 1000.0;
            r.p99_latency_ms = st.p99_us / 1000.0;
            r.min_latency_ms = st.min_us / 1000.0;
            r.max_latency_ms = st.max_us / 1000.0;
            engine_results.results.push_back(r);
        } else {
            std::println("  {:<32}  (skipped)", "Transaction Throughput (5 DML)");
        }

        // -------- T6.4 Isolation Conflict: 故意制造写写冲突，观察 abort 率 --------
        if (scenario_enabled("Isolation Conflict")) {
            rebaseline();
            const std::string scen = "Isolation Conflict (W/W)";
            std::print("  {:<32}", scen);
            std::fflush(stdout);
            bench_ctx().reset_counters();

            const int txns_per_thread = std::clamp(requests_per_thread / 32, 3, 8);
            // 30 行热点 + 不分桶 → 高冲突。仅做正确性演示，QPS 不是关键指标。
            // 不重试，统计 abort 率（QPS 不是关键指标，关键看 first-committer-wins
            // 是否如预期 reject 后到的事务）。
            auto start_time = Clock::now();
            std::vector<std::thread> threads;
            for (int tid = 0; tid < concurrency; ++tid) {
                threads.emplace_back([tid, txns_per_thread]() {
                    if (!wait_for_server("127.0.0.1", kServerPort, 5000))
                        return;
                    socket_t fd = connect_socket("127.0.0.1", kServerPort);
                    if (fd == INVALID_SOCKET_VAL)
                        return;
                    apply_isolation(fd);
                    std::vector<double> local;
                    for (int i = 0; i < txns_per_thread; ++i) {
                        auto t0 = Clock::now();
                        bool failed = false;
                        if (!ok(send_sql_and_recv(fd, "BEGIN")))
                            failed = true;
                        for (int k = 0; k < 3 && !failed; ++k) {
                            // 30 行热点 + 不分桶 → 高冲突但不达 100%
                            int target = 1 + ((tid * 13 + i * 7 + k) % 30);
                            std::string sql = std::format("UPDATE bench_users SET age = {} WHERE id = {}",
                                                          20 + ((i + k) % 60), target);
                            auto resp = send_sql_and_recv(fd, sql);
                            if (!ok(resp)) {
                                failed = true;
                                break;
                            }
                        }
                        if (failed) {
                            send_sql_and_recv(fd, "ROLLBACK");
                        } else if (!ok(send_sql_and_recv(fd, "COMMIT"))) {
                            failed = true;
                        }
                        auto t1 = Clock::now();
                        local.push_back(static_cast<double>(duration_cast<microseconds>(t1 - t0).count()));
                        g_total_requests.fetch_add(1);
                        if (failed)
                            g_failed_requests.fetch_add(1);
                        else
                            g_successful_requests.fetch_add(1);
                    }
                    close_socket(fd);
                    std::lock_guard<std::mutex> lk(g_latency_mutex);
                    g_latencies_us.insert(g_latencies_us.end(), local.begin(), local.end());
                });
            }
            for (auto& t: threads)
                t.join();
            auto end_time = Clock::now();
            double sec = static_cast<double>(duration_cast<milliseconds>(end_time - start_time).count()) / 1000.0;
            int64_t total = g_total_requests.load();
            int64_t success = g_successful_requests.load();
            int64_t aborted = g_failed_requests.load();
            double tps = static_cast<double>(total) / sec;
            LatencyStats st;
            {
                std::lock_guard<std::mutex> lk(g_latency_mutex);
                st = calculate_latency_stats(g_latencies_us);
            }
            std::println("  {:5.2f}s  {:>5} txn  {:>6.0f} TPS  {:>5.1f}%  ({:.1f}% abort)  P50:{:<9} P99:{}",
                         sec, total, tps,
                         100.0 * static_cast<double>(success) / static_cast<double>(std::max<int64_t>(1, total)),
                         100.0 * static_cast<double>(aborted) / static_cast<double>(std::max<int64_t>(1, total)),
                         format_latency(st.p50_us), format_latency(st.p99_us));
            BenchmarkResult r;
            r.test_name = scen;
            r.concurrency = concurrency;
            r.total_requests = total;
            r.success_requests = success;
            r.failed_requests = aborted;
            r.abort_count = aborted;
            r.isolation_level = g_bench_options.isolation_level;
            r.duration_sec = sec;
            r.qps = tps;
            r.avg_latency_ms = st.avg_us / 1000.0;
            r.p50_latency_ms = st.p50_us / 1000.0;
            r.p95_latency_ms = st.p95_us / 1000.0;
            r.p99_latency_ms = st.p99_us / 1000.0;
            r.min_latency_ms = st.min_us / 1000.0;
            r.max_latency_ms = st.max_us / 1000.0;
            engine_results.results.push_back(r);
        } else {
            std::println("  {:<32}  (skipped)", "Isolation Conflict (W/W)");
        }

        // 关闭 setup 连接，停止服务器
        close_socket(setup_fd);
        std::println(std::cerr, "Stopping {} server...", engine_name);
        corodb::request_server_shutdown();
        server_thread.join();

        // 清理 WAL 写入器（释放文件句柄）
        corodb::storage_internal::WalManager::instance().clear_all();

        // 等待端口完全释放（Windows TIME_WAIT 问题）
        std::this_thread::sleep_for(milliseconds(1000));

        // 清理临时目录
        std::filesystem::remove_all(data_dir);

        return engine_results;
    }

} // namespace bench

// ============================================================================
//                          主函数
// ============================================================================

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::println(std::cerr, "WSAStartup failed");
        return 1;
    }
#endif

    std::println("==========================================================");
    std::println("        CoroDB Reactor Server Concurrency Benchmark        ");
    std::println("==========================================================");

    // 解析命令行参数
    int concurrency = kDefaultConcurrency;
    int requests_per_thread = kDefaultRequestsPerThread;
    std::string report_file = "benchmark_report.md";

    auto split_csv = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c: s) {
            if (c == ',') {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--concurrency") && i + 1 < argc) {
            concurrency = std::atoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--requests") && i + 1 < argc) {
            requests_per_thread = std::atoi(argv[++i]);
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            report_file = argv[++i];
        } else if (arg == "--scenario" && i + 1 < argc) {
            bench::g_bench_options.scenarios = split_csv(argv[++i]);
        } else if (arg == "--repeat" && i + 1 < argc) {
            bench::g_bench_options.repeat = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            bench::g_bench_options.warmup = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            bench::g_bench_options.seed = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--isolation" && i + 1 < argc) {
            bench::g_bench_options.isolation_level = argv[++i];
        } else if (arg == "--wal-sync") {
            ++i; // 已在前面解析过，跳过其值
        } else if (arg == "-h" || arg == "--help") {
            std::println("Usage: {} [options]", argv[0]);
            std::println("Options:");
            std::println("  -c, --concurrency N    Number of concurrent clients (default: {})", kDefaultConcurrency);
            std::println("  -n, --requests N       Requests per client (default: {})", kDefaultRequestsPerThread);
            std::println("  -o, --output FILE      Output report file (default: benchmark_report.md)");
            std::println("      --wal-sync MODE    WAL sync mode: none|normal|group|full (default: group)");
            std::println("      --scenario LIST    Comma-separated substring filter for scenario names");
            std::println("      --repeat N         Repeat each engine pass N times, keep best (default: 1)");
            std::println("      --warmup N         Per-scenario warmup requests (default: 0)");
            std::println("      --seed N           RNG seed (0 = use std::time)");
            std::println("      --isolation LEVEL  SET TRANSACTION ISOLATION LEVEL <LEVEL> on every connection");
            std::println("                         (e.g. READ COMMITTED | REPEATABLE READ | SERIALIZABLE)");
            std::println("  -h, --help             Show this help");
            std::println("\nExamples:");
            std::println("  {} -c 10 -n 50              Test with custom settings", argv[0]);
            std::println("  {} -c 30 -o r.md            Custom concurrency and output", argv[0]);
            std::println("  {} --scenario \"Isolation,Throughput\"  Run only matching scenarios", argv[0]);
            std::println("  {} --repeat 3 --warmup 50   3 repeats, 50 warmup ops per scenario", argv[0]);
            return 0;
        }
    }

    // 设置 RNG 种子（用于 data_dir 生成等）
    if (bench::g_bench_options.seed != 0) {
        std::srand(bench::g_bench_options.seed);
    } else {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }

    std::print("  concurrency={}×{} req  total={}  port={}  WAL=GroupCommit",
               concurrency, requests_per_thread, concurrency * requests_per_thread, kServerPort);
    if (!bench::g_bench_options.scenarios.empty()) {
        std::print("  filter=[");
        for (const auto& s: bench::g_bench_options.scenarios)
            std::print("{} ", s);
        std::print("]");
    }
    if (bench::g_bench_options.repeat > 1)
        std::print("  repeat={}x", bench::g_bench_options.repeat);
    if (!bench::g_bench_options.isolation_level.empty())
        std::print("  isolation={}", bench::g_bench_options.isolation_level);
    std::println("");

    // 创建测试报告结构
    TestReport report;
    report.timestamp = get_timestamp();
    report.concurrency = concurrency;
    report.requests_per_thread = requests_per_thread;

    // 测试 LSM 引擎
    std::println("\n  CoroDB  ·  LSM engine  ·  concurrency benchmark");
    std::println("  {:-<60}", "");

    try {
        EngineTestResults best;
        double best_score = -1.0;
        for (int rep = 0; rep < bench::g_bench_options.repeat; ++rep) {
            if (bench::g_bench_options.repeat > 1) {
                std::println("\n>>> Pass {}/{}", rep + 1, bench::g_bench_options.repeat);
            }
            auto pass = run_engine_tests("LSM", concurrency, requests_per_thread);
            double sum_qps = 0.0;
            for (const auto& r: pass.results)
                sum_qps += r.qps;
            double mean_qps = pass.results.empty() ? 0.0 : sum_qps / static_cast<double>(pass.results.size());
            if (mean_qps > best_score) {
                best_score = mean_qps;
                best = std::move(pass);
            }
        }
        report.engine_results.push_back(std::move(best));
    } catch (const std::exception& e) {
        std::println(std::cerr, "LSM engine test failed: {}", e.what());
    }

    // 生成并保存报告
    std::string report_content = generate_markdown_report(report);
    if (!save_report(report_content, report_file)) {
        std::println(std::cerr, "Failed to save report to {}", report_file);
    }

    // 输出摘要
    std::println("\n  {:-<60}", "");
    std::println("  {:<32}  {:>8}  {:>6}  {:>7}  {}", "Scenario", "QPS/TPS", "P99", "Success", "");
    std::println("  {:-<60}", "");

    for (const auto& engine: report.engine_results) {
        for (const auto& r: engine.results) {
            double success_rate =
                    100.0 * static_cast<double>(r.success_requests) / static_cast<double>(r.total_requests);
            std::println("  {:<32}  {:>8.0f}  {:>6}  {:>6.1f}%",
                         r.test_name, r.qps, format_latency(static_cast<int64_t>(r.p99_latency_ms * 1000)),
                         success_rate);
        }
    }

    std::println("\n  Report: {}", report_file);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}

