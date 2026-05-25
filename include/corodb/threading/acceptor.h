// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file acceptor.h @brief 监听与接入连接的接口。 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "corodb/net/port.h"
#include "corodb/threading/event_loop.h"

namespace corodb {

    /** @brief 封装监听 socket，负责接受新连接并回调通知。 */
    class Acceptor {
    public:
        using NewConnectionCallback = std::function<void(socket_t fd, const std::string& peer_addr)>;

        /** @brief 构造 Acceptor。 @param loop 主事件循环。 @param port 监听端口。 @param reuse_port 是否启用端口复用。
         */
        Acceptor(EventLoop* loop, uint16_t port, bool reuse_port = true);

        ~Acceptor();

        /** @brief 禁止复制。 */
        Acceptor(const Acceptor&) = delete;
        Acceptor& operator=(const Acceptor&) = delete;

        /** @brief 设置新连接回调。 */
        void set_new_connection_callback(NewConnectionCallback cb) {
            new_connection_callback_ = std::move(cb);
        }

        /** @brief 开始监听。 */
        void listen();

        /** @brief 是否正在监听。 */
        [[nodiscard]] bool listening() const noexcept {
            return listening_;
        }

    private:
        void handle_read();

        EventLoop* loop_;                  ///< 所属事件循环
        socket_t listen_fd_;               ///< 监听 socket
        std::unique_ptr<Channel> channel_; ///< I/O 通道
        NewConnectionCallback new_connection_callback_;
        bool listening_{ false };
    };

} // namespace corodb
