// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file tls_sql_server.h @brief SQL over TLS 专用监听端口（P2）。
 *
 * 线程/连接阻塞模型：accept 后完成 TLS 握手，逐行读取 SQL（与明文端口相同的
 * @TABLE/@END 文本协议）并在同线程执行。适合加密直连场景；原非阻塞 reactor
 * 端口保持不变（内网或 TLS 终结代理场景）。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "corodb/db/database.h"
#include "corodb/net/tls.h"

namespace corodb {

    class TlsSqlServer {
    public:
        TlsSqlServer(uint16_t port, std::shared_ptr<tls::TlsContext> tls, Database& db);
        ~TlsSqlServer();

        TlsSqlServer(const TlsSqlServer&) = delete;
        TlsSqlServer& operator=(const TlsSqlServer&) = delete;

        /** @brief 开始监听（绑定失败抛 std::runtime_error）。 */
        void start();

        /** @brief 停止监听并断开所有活动连接。 */
        void stop();

        [[nodiscard]] uint16_t port() const noexcept {
            return bound_port_.load();
        }

    private:
        void accept_loop();
        void serve_client(socket_t fd);

        uint16_t port_{ 0 };
        std::shared_ptr<tls::TlsContext> tls_;
        Database& db_;
        socket_t listen_fd_{ INVALID_SOCKET_VAL };
        std::atomic<uint16_t> bound_port_{ 0 };
        std::atomic<bool> running_{ false };
        std::thread accept_thread_;
    };

} // namespace corodb
