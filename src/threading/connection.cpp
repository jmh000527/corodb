// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file connection.cpp
// @brief 客户端 TCP 连接处理的实现。

#include "corodb/threading/connection.h"

#include <cstring>
#include <iostream>

#include "corodb/common/logger.h"

namespace corodb {

    // ============================================================================
    //                          Connection 构造与析构
    // ============================================================================

    /**
     * @brief Connection 构造函数
     * @param loop 所属的事件循环（Sub-Reactor）
     * @param fd 已建立连接的 socket 描述符
     * @param peer_addr 客户端地址字符串 ("IP:Port")
     *
     * 每个 Connection 对象代表一个客户端连接，封装了：
     * - socket I/O 操作
     * - 读写缓冲区管理
     * - 连接状态维护
     */
    Connection::Connection(EventLoop* loop, socket_t fd, std::string peer_addr)
        : loop_(loop), fd_(fd), peer_addr_(std::move(peer_addr)), channel_(std::make_unique<Channel>(loop, fd)) {

        // 注册各类事件的回调函数
        channel_->set_read_callback([this] { handle_read(); });   // 数据可读
        channel_->set_write_callback([this] { handle_write(); }); // 可以写入
        channel_->set_close_callback([this] { handle_close(); }); // 连接关闭
        channel_->set_error_callback([this] { handle_error(); }); // 发生错误

        // 预分配缓冲区，减少后续内存分配开销
        input_buffer_.reserve(4096);  // 接收缓冲区 4KB
        output_buffer_.reserve(4096); // 发送缓冲区 4KB
    }

    /**
     * @brief Connection 析构函数
     *
     * 关闭底层 socket，释放系统资源
     */
    Connection::~Connection() {
        // Ensure the channel is removed from the event loop's channels map.
        // connection_destroyed() normally handles this, but the destructor
        // provides a safety net for paths that skip the normal teardown.
        if (channel_) {
            channel_->remove();
        }
        if (fd_ != INVALID_SOCKET_VAL) {
            close_socket(fd_);
        }
    }

    // ============================================================================
    //                          连接生命周期管理
    // ============================================================================

    /**
     * @brief 连接建立完成
     *
     * 在 I/O 线程中调用，标志连接进入活跃状态
     * 开始监听读事件，并通知上层应用
     */
    void Connection::connection_established() {
        state_.store(ConnectionState::Connected); // 更新状态为已连接
        channel_->enable_reading();               // 开始监听读事件

        // 通知上层应用连接已建立
        if (connection_callback_) {
            connection_callback_(shared_from_this());
        }
    }

    /**
     * @brief 连接销毁
     *
     * 清理连接资源，停止事件监听
     */
    void Connection::connection_destroyed() {
        if (state_.load() == ConnectionState::Connected) {
            state_.store(ConnectionState::Disconnected); // 更新状态
            channel_->disable_all();                     // 停止所有事件监听
        }
        channel_->remove(); // 从事件循环中移除
    }

    // ============================================================================
    //                          数据发送
    // ============================================================================

    /**
     * @brief 发送数据（左值引用版本）
     * @param data 要发送的数据
     *
     * 线程安全：可从任意线程调用
     * - 如果在 I/O 线程中，直接发送
     * - 如果在其他线程，将任务投递到 I/O 线程执行
     */
    void Connection::send(const std::string& data) {
        // 检查连接状态，未连接时忽略发送请求
        if (state_.load() != ConnectionState::Connected) {
            return;
        }

        if (loop_->is_in_loop_thread()) {
            // 已在 I/O 线程，直接发送
            send_in_loop(data);
        } else {
            // 跨线程调用，需要将数据复制到 I/O 线程
            // 使用 shared_from_this() 防止回调执行时对象已销毁
            std::string data_copy = data;
            loop_->run_in_loop([this, self = shared_from_this(), data_copy = std::move(data_copy)]() mutable {
                send_in_loop(data_copy);
            });
        }
    }

    /**
     * @brief 发送数据（右值引用版本）
     * @param data 要发送的数据（移动语义）
     *
     * 避免不必要的数据拷贝，提高性能
     */
    void Connection::send(std::string&& data) {
        if (state_.load() != ConnectionState::Connected) {
            return;
        }

        if (loop_->is_in_loop_thread()) {
            send_in_loop(data);
        } else {
            // 使用移动语义将数据转移到 I/O 线程
            loop_->run_in_loop(
                    [this, self = shared_from_this(), data = std::move(data)]() mutable { send_in_loop(data); });
        }
    }

    /**
     * @brief 在 I/O 线程中执行实际发送
     * @param data 要发送的数据
     *
     * 发送策略：
     * 1. 如果发送缓冲区为空，尝试直接写入 socket（零拷贝优化）
     * 2. 如果一次写不完，剩余数据存入发送缓冲区
     * 3. 注册写事件，等待 socket 可写时继续发送
     */
    void Connection::send_in_loop(const std::string& data) {
        if (state_.load() != ConnectionState::Connected) {
            return;
        }

        std::size_t remaining = data.size(); // 剩余未发送字节数
        std::size_t sent = 0;                // 已发送字节数
        bool fault_error = false;            // 是否发生致命错误

        // ==== 优化：发送缓冲区为空时尝试直接发送 ====
        // 避免将数据先拷贝到缓冲区再发送
        if (write_offset_ >= output_buffer_.size()) {
            // 缓冲区已全部发送，先紧凑化再尝试直接发送
            output_buffer_.clear();
            write_offset_ = 0;
            int n = write_socket(fd_, data.data(), data.size());
            if (n >= 0) {
                // 发送成功，记录已发送字节数
                sent = static_cast<std::size_t>(n);
                remaining = data.size() - sent;
            } else {
                // 发送失败，检查错误类型
#ifdef _WIN32
                int err = WSAGetLastError();
                // WSAEWOULDBLOCK：缓冲区满，需等待，非致命错误
                if (err != WSAEWOULDBLOCK) {
                    fault_error = true;
                }
#else
                // EAGAIN/EWOULDBLOCK：缓冲区满，需等待
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // EPIPE：对端关闭写入端
                    // ECONNRESET：连接被重置
                    if (errno == EPIPE || errno == ECONNRESET) {
                        fault_error = true;
                    }
                }
#endif
            }
        }

        // 致命错误（EPIPE / ECONNRESET），关闭连接
        if (fault_error) {
            handle_close();
            return;
        }

        // ==== 处理剩余数据 ====
        if (remaining > 0) {
            // 将未发送的数据追加到发送缓冲区
            output_buffer_.append(data.data() + sent, remaining);
            // Check output buffer limit to prevent memory exhaustion.
            if (output_buffer_.size() > max_output_buffer_size_) {
                LOG_WARN("Connection output buffer limit exceeded for {}", peer_addr_);
                handle_close();
                return;
            }
            // 注册写事件，socket 可写时触发 handle_write()
            if (!channel_->is_writing()) {
                channel_->enable_writing();
            }
        }
    }

    // ============================================================================
    //                          连接关闭
    // ============================================================================

    /**
     * @brief 优雅关闭连接
     *
     * 半关闭（half-close）：关闭写端，但仍可接收数据
     * 等待发送缓冲区数据发送完毕后再关闭
     */
    void Connection::shutdown() {
        if (state_.load() != ConnectionState::Connected) {
            return;
        }
        // 进入"正在断开"状态
        state_.store(ConnectionState::Disconnecting);

        // 在 I/O 线程中执行关闭操作
        loop_->run_in_loop([this, self = shared_from_this()]() { shutdown_in_loop(); });
    }

    /**
     * @brief 在 I/O 线程中执行关闭
     *
     * 只有当发送缓冲区为空时才关闭写端
     * 否则等待 handle_write() 发送完毕后再关闭
     */
    void Connection::shutdown_in_loop() {
        if (!channel_->is_writing()) {
            // 发送缓冲区已空，执行半关闭
#ifdef _WIN32
            ::shutdown(fd_, SD_SEND); // Windows: 关闭发送端
#else
            ::shutdown(fd_, SHUT_WR); // Linux: 关闭写端
#endif
        }
        // 如果还有数据待发送，handle_write() 发送完后会调用此函数
    }

    /**
     * @brief 强制关闭连接
     *
     * 立即关闭，不等待发送缓冲区清空
     */
    void Connection::force_close() {
        if (state_.load() == ConnectionState::Connected || state_.load() == ConnectionState::Disconnecting) {
            handle_close();
        }
    }

    // ============================================================================
    //                          事件处理
    // ============================================================================

    /**
     * @brief 处理读事件
     *
     * 当 socket 可读时被调用
     * 读取数据到接收缓冲区，然后调用消息回调处理
     */
    void Connection::handle_read() {
        char buf[65536]; // 64KB 临时读取缓冲区
        int n = read_socket(fd_, buf, sizeof(buf));

        if (n > 0) {
            // ==== 成功读取数据 ====
            touch();
            input_buffer_.append(buf, static_cast<std::size_t>(n));
            // Check input buffer limit to prevent memory exhaustion DoS.
            if (input_buffer_.size() > max_input_buffer_size_) {
                LOG_WARN("Connection input buffer limit exceeded from {}", peer_addr_);
                handle_close();
                return;
            }
            // 调用消息回调，由上层应用处理数据
            if (message_callback_) {
                message_callback_(shared_from_this(), input_buffer_);
            }
        } else if (n == 0) {
            // ==== 对端关闭连接 ====
            // read() 返回 0 表示 EOF，对端执行了 close() 或 shutdown()
            handle_close();
        } else {
            // ==== 读取错误 ====
#ifdef _WIN32
            int err = WSAGetLastError();
            // WSAEWOULDBLOCK：暂无数据可读，非阻塞模式正常情况
            if (err != WSAEWOULDBLOCK) {
                handle_error();
            }
#else
            // EAGAIN/EWOULDBLOCK：暂无数据可读
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                handle_error();
            }
#endif
        }
    }

    /**
     * @brief 处理写事件
     *
     * 当 socket 可写时被调用
     * 将发送缓冲区的数据写入 socket
     */
    void Connection::handle_write() {
        if (!channel_->is_writing()) {
            return; // 未注册写事件，忽略
        }

        // 尝试将发送缓冲区数据写入 socket
        int n = write_socket(fd_, output_buffer_.data() + write_offset_, output_buffer_.size() - write_offset_);
        if (n > 0) {
            // 写入成功，推进已发送偏移量
            write_offset_ += static_cast<std::size_t>(n);

            if (write_offset_ >= output_buffer_.size()) {
                // 发送缓冲区已全部发送，紧凑化缓冲区
                output_buffer_.clear();
                write_offset_ = 0;
                channel_->disable_writing(); // 取消写事件监听

                // 如果处于正在断开状态，执行关闭
                if (state_.load() == ConnectionState::Disconnecting) {
                    shutdown_in_loop();
                }
            }
        } else {
            // 写入失败
#ifdef _WIN32
            int err = WSAGetLastError();
            // WSAEWOULDBLOCK：缓冲区满，等待下次可写
            if (err != WSAEWOULDBLOCK) {
                LOG_WARN("Connection::handle_write error");
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_WARN("Connection::handle_write error");
            }
#endif
        }
    }

    /**
     * @brief 处理连接关闭事件
     *
     * 更新状态、停止事件监听、通知上层应用
     */
    void Connection::handle_close() {
        state_.store(ConnectionState::Disconnected); // 更新为已断开状态
        channel_->disable_all();                     // 停止所有事件监听

        // 通知上层应用连接已关闭
        // 使用 shared_from_this() 确保回调期间对象不被销毁
        auto self = shared_from_this();
        if (close_callback_) {
            close_callback_(self);
        }
    }

    /**
     * @brief 处理错误事件
     *
     * 记录错误信息（生产环境可扩展为更详细的错误处理）
     */
    void Connection::handle_error() {
        LOG_WARN("Connection::handle_error from {}", peer_addr_);
        handle_close();
    }

} // namespace corodb
