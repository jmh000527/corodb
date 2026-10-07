// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file server.cpp
// @brief Reactor 模式 TCP SQL 服务器的实现。

#include "corodb/server/server.h"

#include "corodb/common/metrics.h"
#include "corodb/db/database.h"
#include "corodb/db/session.h"
#include "corodb/executor/executor.h"
#include "corodb/net/port.h"
#include "corodb/replication/replication.h"
#include "corodb/server/admin_server.h"
#include "corodb/server/sql_protocol.h"
#include "corodb/server/tls_sql_server.h"
#include "corodb/net/tls.h"
#include "corodb/storage/lsm_storage_engine.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/threading/reactor_server.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <variant>

#include "corodb/common/logger.h"

namespace corodb {

    // ---- 全局服务器状态 ----

    namespace {
        std::atomic<bool> g_stop_requested{ false }; ///< 停止请求标志
        std::atomic<bool> g_server_running{ false }; ///< 服务器运行状态
        ReactorServer* g_server_ptr{ nullptr };      ///< 当前服务器实例指针
        std::mutex g_server_mutex;                   ///< 服务器状态互斥锁

        Database* g_shared_db{ nullptr }; ///< 共享数据库实例（线程安全）

        // ---- 预注册指标（引用在首次调用时惰性初始化，进程内稳定） ----

        Counter& queries_ok() {
            return Metrics::instance().counter("corodb_queries_total", R"({result="ok"})",
                                               "SQL statements executed successfully.");
        }
        Counter& queries_error() {
            return Metrics::instance().counter("corodb_queries_total", R"({result="error"})",
                                               "SQL statements executed successfully.");
        }
        Histogram& query_duration() {
            return Metrics::instance().histogram(
                    "corodb_query_duration_seconds",
                    {0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0},
                    "SQL statement execution duration in seconds.");
        }
        Counter& slow_queries() {
            return Metrics::instance().counter("corodb_slow_queries_total", {},
                                               "SQL statements exceeding the slow query threshold.");
        }
        Gauge& connections_active() {
            return Metrics::instance().gauge("corodb_connections_active", {},
                                             "Currently connected clients.");
        }
        Counter& connections_total() {
            return Metrics::instance().counter("corodb_connections_total", {},
                                               "Client connections accepted since startup.");
        }
    } // namespace

    void request_server_shutdown() {
        g_stop_requested.store(true);

        std::lock_guard lock(g_server_mutex);
        if (g_server_ptr) {
            g_server_ptr->stop();
        }
    }

    bool is_server_running() {
        return g_server_running.load();
    }

    namespace {

        /**
         * @brief 消息处理回调
         *
         * 在 I/O 线程中调用，将 SQL 执行任务提交到工作线程池。
         * 每个连接的 Session 通过 Connection::user_data 持有，
         * 保证多连接并发时事务状态彼此隔离（修复 B1 的真正完整解法）。
         */
        void on_message(ReactorServer& server, Database& db, const ConnectionPtr& conn, std::string& buffer) {
            // Lazy idle-connection check every ~128 messages.
            static thread_local unsigned idle_check_counter = 0;
            if ((++idle_check_counter & 127) == 0) {
                server.check_idle_connections();
            }

            // 惰性初始化连接级 Session。Connection 不暴露线程安全 API，
            // 但 user_data 只在 I/O 线程中读写（on_message 由 I/O 线程派发），
            // 所以这里无需额外锁。
            std::shared_ptr<Session> session;
            if (auto raw = conn->user_data()) {
                session = std::static_pointer_cast<Session>(raw);
            } else {
                session = std::make_shared<Session>();
                session->statement_timeout_ms = Config::instance().statement_timeout_ms();
                conn->set_max_input_buffer_size(Config::instance().max_buffer_size());
                conn->set_max_output_buffer_size(Config::instance().max_buffer_size());
                conn->set_user_data(session);
            }

            // 逐行处理
            for (;;) {
                auto pos = buffer.find('\n');
                if (pos == std::string::npos)
                    break;

                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                if (line.empty() ||
                    std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); })) {
                    continue;
                }

                // 在工作线程中执行 SQL。同一连接的 SQL 串行执行（按到达顺序
                // 提交到线程池，且事务语义要求顺序执行）。
                server.post_task([&db, conn, session, line = std::move(line)]() {
                    // P4 限流：令牌桶（每连接；[server].rate_limit_per_sec）。
                    const uint32_t rate = Config::instance().rate_limit_per_sec();
                    if (rate > 0) {
                        const uint64_t now_ns = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
                        // 补充令牌：按流逝时间线性补给，封顶速率值（允许小额突发）。
                        if (session->rate_last_refill_ns != 0 &&
                            now_ns > session->rate_last_refill_ns) {
                            const uint64_t elapsed = now_ns - session->rate_last_refill_ns;
                            const uint64_t gained = elapsed * rate / 1'000'000'000ull;
                            if (gained > 0) {
                                session->rate_tokens = static_cast<uint32_t>(
                                        std::min<uint64_t>(rate, session->rate_tokens + gained));
                                session->rate_last_refill_ns = now_ns;
                            }
                        } else {
                            session->rate_tokens = rate; // 首语句：满桶
                            session->rate_last_refill_ns = now_ns;
                        }
                        if (session->rate_tokens == 0) {
                            conn->send(std::string("ERROR: rate limit exceeded (") +
                                       std::to_string(rate) + " stmts/s)\n@END\n");
                            return;
                        }
                        session->rate_tokens--;
                    }
                    session->statements_executed++;
                    std::string response = sql_proto::process_sql_line(db, line, session);
                    if (!response.empty()) {
                        conn->send(std::move(response));
                    }
                });
            }
        }

    } // namespace

    /**
     * @brief 运行 Reactor 模式服务器
     */
    int run_server(const ServerConfig& cfg) {
#ifndef _WIN32
        ::signal(SIGPIPE, SIG_IGN);
#else
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            LOG_ERROR("WSAStartup failed");
            return 1;
        }
#endif

        // 重置停止标志
        g_stop_requested.store(false);
        g_server_running.store(true);

        // WAL sync mode from config
        if (Config::instance().wal_sync_mode() == "durable")
            storage_internal::set_wal_sync_mode(storage_internal::WalSyncMode::Durable);

        // 配置 Reactor 服务器
        ReactorServerConfig reactor_cfg;
        reactor_cfg.port = cfg.port;
        reactor_cfg.io_threads = cfg.io_threads;
        reactor_cfg.worker_threads = cfg.worker_threads;
        reactor_cfg.max_connections = cfg.max_connections;
        reactor_cfg.reuse_port = cfg.reuse_port;
        reactor_cfg.idle_timeout = std::chrono::seconds(cfg.idle_timeout_sec);
        reactor_cfg.thread_pool_max_queue = cfg.thread_pool_max_queue;

        // 初始化数据库实例
        Database db(cfg.data_dir);
        g_shared_db = &db;

        // ---- TLS 传输加密（P2）：管理端 / 复制通道 / SQL TLS 专用端口 ----
        std::shared_ptr<tls::TlsContext> tls_server_ctx; // 服务器角色上下文
        std::shared_ptr<tls::TlsContext> tls_client_ctx; // 客户端角色上下文（复制从端）
        if (Config::instance().tls_enabled()) {
            tls::TlsConfig tcfg;
            tcfg.cert_path = Config::instance().tls_cert_path();
            tcfg.key_path = Config::instance().tls_key_path();
            tcfg.ca_path = Config::instance().tls_ca_path();
            tcfg.verify_cert = Config::instance().tls_verify_cert();
            try {
                tls_server_ctx = tls::TlsContext::create_server(tcfg);
                LOG_INFO("TLS enabled (server context ok)");
            } catch (const std::exception& ex) {
                LOG_WARN("TLS server context failed: {}", ex.what());
            }
        }

        // 管理端 HTTP（/metrics、/healthz；P2 观测性）。仅绑定本机回环。
        std::unique_ptr<AdminServer> admin;
        if (Config::instance().metrics_enabled()) {
            const auto server_start = std::chrono::steady_clock::now();
            auto& uptime_gauge =
                    Metrics::instance().gauge("corodb_uptime_seconds", {}, "Server uptime in seconds.");
            // 预注册指标：首 scrape 前即有零值序列。
            Metrics::instance().counter("corodb_rows_written_total", {},
                                        "Rows inserted/updated/deleted since startup.");
            Metrics::instance().counter("corodb_txn_committed_total", {}, "Committed transactions since startup.");
            Metrics::instance().counter("corodb_txn_aborted_total", {},
                                        "Rolled back or aborted transactions since startup.");
            Metrics::instance().gauge("corodb_txn_active", {}, "Currently active transactions.");
            AdminServer::Options admin_opts;
            admin_opts.port = Config::instance().metrics_port();
            admin_opts.tls = tls_server_ctx;
            admin_opts.handler = [&server_start, &uptime_gauge](const std::string& path) {
                if (path == "/healthz")
                    return std::string("ok\n");
                if (path == "/metrics") {
                    uptime_gauge.set(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::steady_clock::now() - server_start)
                                             .count());
                    return Metrics::instance().render_prometheus();
                }
                return std::string();
            };
            admin = std::make_unique<AdminServer>(std::move(admin_opts));
            try {
                admin->start();
                LOG_INFO("Admin endpoint: http://127.0.0.1:{}/metrics", admin->port());
            } catch (const std::exception& ex) {
                LOG_WARN("Admin endpoint disabled: {}", ex.what());
                admin.reset(); // 指标端点失败不阻断主服务
            }
        }

        // ---- WAL 日志复制（P4 主从） ----
        ReplicationHub repl_hub;          // primary：复制日志集线器
        ReplicationFollower repl_follower; // replica：日志接收器
        const std::string repl_role = Config::instance().replication_role();
        if (repl_role == "replica") {
            db.set_read_only(true);
            if (Config::instance().tls_enabled()) {
                tls::TlsConfig tcfg;
                tcfg.cert_path = Config::instance().tls_cert_path();
                tcfg.key_path = Config::instance().tls_key_path();
                tcfg.ca_path = Config::instance().tls_ca_path();
                tcfg.verify_cert = Config::instance().tls_verify_cert();
                try {
                    tls_client_ctx = tls::TlsContext::create_client(tcfg);
                } catch (const std::exception& ex) {
                    LOG_WARN("TLS client context failed: {}", ex.what());
                }
            }
            repl_follower.set_tls(tls_client_ctx);
            // 解析 connect = "host:port"（缺省 127.0.0.1:[replication].port）。
            const std::string connect = Config::instance().replication_connect();
            std::string repl_host = "127.0.0.1";
            uint16_t repl_port = Config::instance().replication_port();
            if (const auto colon = connect.rfind(':'); colon != std::string::npos) {
                repl_host = connect.substr(0, colon);
                try {
                    const int parsed = std::stoi(connect.substr(colon + 1));
                    if (parsed > 0 && parsed <= 65535)
                        repl_port = static_cast<uint16_t>(parsed);
                } catch (...) {
                    // 端口非法：保留默认值。
                }
            } else if (!connect.empty()) {
                repl_host = connect;
            }
            auto* lsm = static_cast<LSMTreeEngine*>(db.get_storage());
            repl_follower.start(repl_host, repl_port, [&db, lsm](const ReplicationRecord& rec) {
                // DDL 记录：先应用再刷新 Catalog 使新表对查询可见。
                if (lsm->apply_replication_record(rec) &&
                    (rec.type == ReplicationRecord::Type::CreateTable ||
                     rec.type == ReplicationRecord::Type::DropTable)) {
                    db.reload_catalog();
                    return;
                }
                if (rec.type != ReplicationRecord::Type::CreateTable &&
                    rec.type != ReplicationRecord::Type::DropTable) {
                    LOG_WARN("Replication record for unknown table '{}' skipped", rec.table);
                }
            });
            LOG_INFO("Replica mode: streaming from {}:{} (read-only)", repl_host, repl_port);
        } else {
            auto* lsm = static_cast<LSMTreeEngine*>(db.get_storage());
            lsm->set_replication_sink([&repl_hub](const ReplicationRecord& rec) { repl_hub.broadcast(rec); });
            repl_hub.set_tls(tls_server_ctx);
            try {
                repl_hub.start(Config::instance().replication_port());
                LOG_INFO("Replication server listening on port {}", repl_hub.port());
            } catch (const std::exception& ex) {
                LOG_WARN("Replication server disabled: {}", ex.what());
            }
        }

        // ---- SQL over TLS 专用端口（P2；线程/连接，复用文本协议） ----
        std::unique_ptr<TlsSqlServer> tls_sql;
        if (Config::instance().tls_enabled() && tls_server_ctx) {
            tls_sql = std::make_unique<TlsSqlServer>(Config::instance().tls_sql_port(), tls_server_ctx, db);
            try {
                tls_sql->start();
                LOG_INFO("SQL TLS listener on port {}", tls_sql->port());
            } catch (const std::exception& ex) {
                LOG_WARN("SQL TLS listener disabled: {}", ex.what());
                tls_sql.reset();
            }
        }

        std::size_t actual_workers = reactor_cfg.worker_threads;
        if (actual_workers == 0) {
            actual_workers = std::thread::hardware_concurrency();
            if (actual_workers == 0)
                actual_workers = 4;
        }
        LOG_INFO("Database ready: {} worker threads, data_dir={}", actual_workers, cfg.data_dir);

        try {
            ReactorServer server(reactor_cfg);

            // 设置全局指针用于 shutdown
            {
                std::lock_guard lock(g_server_mutex);
                g_server_ptr = &server;
            }

            // 设置消息回调
            server.set_message_callback([&server, &db](const ConnectionPtr& conn, std::string& buffer) {
                on_message(server, db, conn, buffer);
            });

            // Rollback active transactions on client disconnect to prevent
            // zombie transactions and leaked row locks.
            server.set_close_callback([&db](const ConnectionPtr& conn) {
                connections_active().decrement();
                auto raw = conn->user_data();
                if (!raw)
                    return;
                auto session = std::static_pointer_cast<Session>(raw);
                if (!session->in_transaction())
                    return;
                uint64_t txn_id = session->current_txn_id;
                try {
                    db.get_txn_manager().rollback(txn_id);
                } catch (...) {
                    // Best-effort cleanup on abnormal disconnect.
                }
                db.get_row_locks().release_all(txn_id);
                session->write_buffer.clear();
                session->read_set.clear();
                session->table_read_versions.clear();
                session->current_txn_id = 0;
            });

            // 设置连接回调（观测：连接数指标）
            server.set_connection_callback([](const ConnectionPtr&) {
                connections_active().increment();
                connections_total().increment();
            });

            // 启动服务器（阻塞）
            server.start();

            // 清理全局指针
            {
                std::lock_guard lock(g_server_mutex);
                g_server_ptr = nullptr;
            }

        } catch (const std::exception& ex) {
            LOG_ERROR("Server error: {}", ex.what());
            g_server_running.store(false);
            g_shared_db = nullptr;
            admin.reset();
            tls_sql.reset();
            repl_follower.stop();
            repl_hub.stop();

            {
                std::lock_guard lock(g_server_mutex);
                g_server_ptr = nullptr;
            }

#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }

        admin.reset();
        tls_sql.reset();
        repl_follower.stop();
        repl_hub.stop();
        g_shared_db = nullptr;
        g_server_running.store(false);

#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

} // namespace corodb
