// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file admin_server.h @brief 管理端 HTTP 服务（Prometheus /metrics 与 /healthz）。
 *
 * 独立于 SQL 端口的最小 HTTP/1.1 只读监听器：单 accept 线程，每连接内联处理
 * （ scrape 流量极小）。路由由调用方注入（路径 → 响应体），未匹配返回 404。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "corodb/net/port.h"

namespace corodb {

    class AdminServer {
    public:
        using Handler = std::function<std::string(const std::string& path)>;

        struct Options {
            uint16_t port{ 4100 }; ///< 监听端口（0 = 由操作系统分配，port() 查询实际值）。
            Handler handler;       ///< 路径 → 响应体（200）；返回空串 → 404。
        };

        explicit AdminServer(Options opt);
        ~AdminServer();

        AdminServer(const AdminServer&) = delete;
        AdminServer& operator=(const AdminServer&) = delete;

        /** @brief 启动 accept 线程；绑定失败抛 std::runtime_error。 */
        void start();

        /** @brief 停止并回收线程（析构自动调用）。 */
        void stop();

        /** @brief 实际监听端口（start 后有效）。 */
        [[nodiscard]] uint16_t port() const noexcept {
            return bound_port_;
        }

    private:
        void accept_loop();
        void handle_client(socket_t fd);

        Options opts_;
        socket_t listen_fd_{ INVALID_SOCKET_VAL };
        std::atomic<uint16_t> bound_port_{ 0 };
        std::atomic<bool> running_{ false };
        std::thread accept_thread_;
    };

} // namespace corodb
