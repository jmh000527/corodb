// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file connection.h @brief TCP 连接对象定义。 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "corodb/net/port.h"
#include "corodb/threading/event_loop.h"

namespace corodb {

    class Connection;
    using ConnectionPtr = std::shared_ptr<Connection>;
    using ConnectionCallback = std::function<void(const ConnectionPtr&)>;
    using MessageCallback = std::function<void(const ConnectionPtr&, std::string&)>;
    using CloseCallback = std::function<void(const ConnectionPtr&)>;

    /** @brief 连接状态。 */
    enum class ConnectionState {
        Disconnected,  ///< 未连接
        Connecting,    ///< 连接中
        Connected,     ///< 已连接
        Disconnecting, ///< 断开中
    };

    /** @brief TCP 连接，管理单个客户端的非阻塞读写与缓冲区。 */
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        /** @brief 构造连接。 @param loop 所属事件循环。 @param fd 客户端 socket。 @param peer_addr 对端地址字符串。 */
        Connection(EventLoop* loop, socket_t fd, std::string peer_addr);

        ~Connection();

        // 禁止复制
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        /** @brief 获取所属事件循环。 */
        [[nodiscard]] EventLoop* get_loop() const noexcept {
            return loop_;
        }

        /** @brief 获取对端地址。 */
        [[nodiscard]] const std::string& peer_address() const noexcept {
            return peer_addr_;
        }

        /** @brief 获取连接状态。 */
        [[nodiscard]] ConnectionState state() const noexcept {
            return state_.load();
        }

        /** @brief 检查是否已连接。 */
        [[nodiscard]] bool connected() const noexcept {
            return state_.load() == ConnectionState::Connected;
        }

        /** @brief 设置连接回调。 */
        void set_connection_callback(ConnectionCallback cb) {
            connection_callback_ = std::move(cb);
        }

        /** @brief 设置消息回调。 */
        void set_message_callback(MessageCallback cb) {
            message_callback_ = std::move(cb);
        }

        /** @brief 设置关闭回调。 */
        void set_close_callback(CloseCallback cb) {
            close_callback_ = std::move(cb);
        }

        /** @brief 连接建立时调用，须在 EventLoop 线程中执行。 */
        void connection_established();

        /** @brief 连接销毁时调用，须在 EventLoop 线程中执行。 */
        void connection_destroyed();

        /** @brief 线程安全地发送数据。 @param data 要发送的数据。 */
        void send(const std::string& data);
        void send(std::string&& data);

        /** @brief 主动关闭连接（半关闭）。 */
        void shutdown();

        /** @brief 强制关闭连接。 */
        void force_close();

        /** @brief 设置接收缓冲区最大大小（超过则断开连接，默认 64MB）。 */
        void set_max_input_buffer_size(std::size_t sz) noexcept {
            max_input_buffer_size_ = sz;
        }

        /** @brief 设置发送缓冲区最大大小（超过则断开连接，默认 64MB）。 */
        void set_max_output_buffer_size(std::size_t sz) noexcept {
            max_output_buffer_size_ = sz;
        }

        /** @brief 设置空闲超时（秒，0 = 禁用）。 */
        void set_idle_timeout(std::chrono::seconds timeout) noexcept {
            idle_timeout_ = timeout;
        }

        /** @brief 更新最后活跃时间（收到数据或发送数据时调用）。 */
        void touch() noexcept {
            last_active_ = std::chrono::steady_clock::now();
        }

        /** @brief 若连接空闲超时则返回 true（调用方应关闭连接）。 */
        [[nodiscard]] bool is_idle_timeout() const noexcept {
            if (idle_timeout_.count() <= 0)
                return false;
            return std::chrono::steady_clock::now() - last_active_ > idle_timeout_;
        }

        /** @brief 设置连接私有数据（类型擦除，调用方负责强转）。 */
        void set_user_data(std::shared_ptr<void> data) noexcept {
            user_data_ = std::move(data);
        }

        [[nodiscard]] const std::shared_ptr<void>& user_data() const noexcept {
            return user_data_;
        }

    private:
        void handle_read();
        void handle_write();
        void handle_close();
        void handle_error();

        void send_in_loop(const std::string& data);
        void shutdown_in_loop();

        EventLoop* loop_;                  ///< 所属事件循环
        socket_t fd_;                      ///< 客户端 socket
        std::string peer_addr_;            ///< 对端地址
        std::unique_ptr<Channel> channel_; ///< I/O 通道
        std::atomic<ConnectionState> state_{ ConnectionState::Connecting };

        // 缓冲区
        static constexpr std::size_t kDefaultMaxBufferSize = 64 * 1024 * 1024; ///< 64 MB
        std::string input_buffer_;      ///< 接收缓冲区
        std::string output_buffer_;     ///< 发送缓冲区
        std::size_t write_offset_{ 0 }; ///< 发送缓冲区已发送偏移量（避免 O(N) erase）
        std::size_t max_input_buffer_size_{ kDefaultMaxBufferSize };
        std::size_t max_output_buffer_size_{ kDefaultMaxBufferSize };

        /// 空闲超时：0 = 禁用（默认）。通过 last_active_ 追踪。
        std::chrono::steady_clock::time_point last_active_{ std::chrono::steady_clock::now() };
        std::chrono::seconds idle_timeout_{ 0 };

        // 回调
        ConnectionCallback connection_callback_;
        MessageCallback message_callback_;
        CloseCallback close_callback_;

        // 连接私有数据（如会话上下文）。类型擦除，调用方负责强转。
        std::shared_ptr<void> user_data_;
    };

} // namespace corodb
