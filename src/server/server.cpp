// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file server.cpp
// @brief Reactor 模式 TCP SQL 服务器的实现。

#include "corodb/server/server.h"

#include "corodb/db/database.h"
#include "corodb/db/session.h"
#include "corodb/executor/executor.h"
#include "corodb/net/port.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/threading/reactor_server.h"

#include <atomic>
#include <cctype>
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
         * @brief 执行 SQL 语句并返回格式化的结果字符串
         * @param db 数据库实例
         * @param sql SQL 语句
         * @param session 当前连接的会话状态（含事务 ID 等）
         * @return 格式化的结果字符串
         */
        std::string run_sql(Database& db, const std::string& sql, std::shared_ptr<Session> session) {
            std::string res;
            const std::string indent = "  ";

            auto result = db.execute(sql, std::move(session));

            if (result.message) {
                std::istringstream iss(*result.message);
                std::string line;
                while (std::getline(iss, line)) {
                    if (!line.empty()) {
                        res += indent;
                        res += line;
                        res += '\n';
                    }
                }
            } else if (result.rows) {
                if (result.is_select) {
                    res += "@TABLE\n";

                    auto value_to_string = [](const Value& v) {
                        if (std::holds_alternative<NullValue>(v))
                            return std::string("NULL");
                        if (std::holds_alternative<int64_t>(v))
                            return std::to_string(std::get<int64_t>(v));
                        if (std::holds_alternative<double>(v))
                            return std::to_string(std::get<double>(v));
                        return std::get<std::string>(v);
                    };

                    auto sanitize = [](std::string s) {
                        for (char& ch: s) {
                            if (ch == '\t' || ch == '\n' || ch == '\r')
                                ch = ' ';
                        }
                        return s;
                    };

                    bool have_header = false;

                    for (const auto& rec: *result.rows) {
                        if (!have_header) {
                            for (std::size_t i = 0; i < rec.bindings.size(); ++i) {
                                if (i > 0)
                                    res += '\t';
                                const auto& b = rec.bindings[i];
                                std::string name = b.column.empty() ? ("col" + std::to_string(i + 1)) : b.column;
                                res += sanitize(name);
                            }
                            res += '\n';
                            have_header = true;
                        }

                        for (std::size_t i = 0; i < rec.values.size(); ++i) {
                            if (i > 0)
                                res += '\t';
                            res += sanitize(value_to_string(rec.values[i]));
                        }
                        res += '\n';
                    }
                } else {
                    for (const auto& _: *result.rows) {
                        (void)_;
                    }
                    res += indent;
                    res += "OK\n";
                }
            }

            auto start = res.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                res.erase(0, start);
            } else {
                res.clear();
            }

            if (!res.empty()) {
                if (!res.starts_with("@TABLE")) {
                    res.insert(0, indent);
                }
            } else {
                res = indent + "OK\n";
            }

            res += "@END\n";
            return res;
        }

        /**
         * @brief 处理单行 SQL
         */
        std::string process_sql_line(Database& db, std::string line, std::shared_ptr<Session> session) {
            // Trim whitespace
            auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
            auto start = std::find_if_not(line.begin(), line.end(), is_ws);
            auto end = std::find_if_not(line.rbegin(), std::make_reverse_iterator(start), is_ws).base();

            if (start != line.begin() || end != line.end()) {
                line = std::string(start, end);
            }

            // Remove trailing semicolon
            if (!line.empty() && line.back() == ';') {
                line.pop_back();
                start = std::find_if_not(line.begin(), line.end(), is_ws);
                end = std::find_if_not(line.rbegin(), std::make_reverse_iterator(start), is_ws).base();
                if (start != line.begin() || end != line.end()) {
                    line = std::string(start, end);
                }
            }

            if (line.empty()) {
                return "";
            }

            try {
                // Database 内部已实现线程安全（读写锁）
                return run_sql(db, line, std::move(session));
            } catch (const WriteConflictError& ex) {
                // 写写冲突是事务并发的预期行为，不打印到 stderr
                return std::string("ERROR: ") + ex.what() + "\n@END\n";
            } catch (const std::exception& ex) {
                LOG_ERROR("Error executing query: {}", ex.what());
                // 注意：保持 "ERROR:" 前缀（大写），客户端必须用
                // is_error_response() 判定，避免大小写不一致（B2）。
                // 必须追加 @END 终止标记，否则客户端 read_response 会等满 30s 超时
                return std::string("ERROR: ") + ex.what() + "\n@END\n";
            }
        }

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
                    std::string response = process_sql_line(db, line, session);
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

            // 设置连接回调（禁用日志以减少噪音）
            // server.set_connection_callback(
            //     [](const ConnectionPtr& conn) {
            //         std::println("Client connected: {}", conn->peer_address());
            //     }
            // );

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

            {
                std::lock_guard lock(g_server_mutex);
                g_server_ptr = nullptr;
            }

#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }

        g_shared_db = nullptr;
        g_server_running.store(false);

#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

} // namespace corodb
