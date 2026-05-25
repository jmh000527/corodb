// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file acceptor.cpp
// @brief TCP 连接接受器的实现。

#include "corodb/threading/acceptor.h"

#include <cstring>
#include <format>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace corodb {

    // ============================================================================
    //                          Acceptor 构造与析构
    // ============================================================================

    /**
     * @brief Acceptor 构造函数
     * @param loop 所属的事件循环（通常是 Main Reactor）
     * @param port 监听端口号
     * @param reuse_port 是否启用端口复用（允许多进程监听同一端口）
     */
    Acceptor::Acceptor(EventLoop* loop, uint16_t port, bool reuse_port) : loop_(loop) {

        // ==== 步骤1：创建监听 socket ====
        // 使用 IPv4 + TCP 协议
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == INVALID_SOCKET_VAL) {
            throw std::runtime_error("Failed to create listen socket");
        }

        // ==== 步骤2：设置 socket 选项 ====
#ifdef _WIN32
        // Windows 平台
        char opt = 1;
        // SO_REUSEADDR：允许重用处于 TIME_WAIT 状态的地址
        // 这对于服务器快速重启非常重要
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (reuse_port) {
            // Windows 不直接支持 SO_REUSEPORT，忽略此选项
        }
#else
        // Linux/Unix 平台
        int opt = 1;
        // SO_REUSEADDR：允许重用处于 TIME_WAIT 状态的地址
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (reuse_port) {
#ifdef SO_REUSEPORT
            // SO_REUSEPORT：允许多个 socket 绑定同一端口
            // 适用于多进程服务器，内核会自动负载均衡
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
        }
#endif

        // ==== 步骤3：绑定地址 ====
        sockaddr_in addr{};
        addr.sin_family = AF_INET;         // IPv4
        addr.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡
        addr.sin_port = htons(port);       // 端口号（网络字节序）

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(listen_fd_);
            throw std::runtime_error("Failed to bind to port " + std::to_string(port));
        }

        // ==== 步骤4：设置非阻塞模式 ====
        // 非阻塞模式下 accept() 不会阻塞，配合 epoll/select 使用
        set_nonblocking(listen_fd_);

        // ==== 步骤5：创建 Channel 并注册读事件回调 ====
        // 当有新连接到达时，listen_fd_ 变为可读
        channel_ = std::make_unique<Channel>(loop_, listen_fd_);
        channel_->set_read_callback([this] { handle_read(); });
    }

    /**
     * @brief Acceptor 析构函数
     *
     * 清理资源：禁用事件监听、从事件循环移除、关闭 socket
     */
    Acceptor::~Acceptor() {
        channel_->disable_all();  // 禁用所有事件监听
        channel_->remove();       // 从 EventLoop 中移除
        close_socket(listen_fd_); // 关闭监听 socket
    }

    // ============================================================================
    //                          监听与连接处理
    // ============================================================================

    /**
     * @brief 开始监听连接
     *
     * 调用后服务器进入监听状态，可以接受客户端连接
     */
    void Acceptor::listen() {
        listening_ = true;

        // 开始监听，SOMAXCONN 是系统允许的最大等待连接队列长度
        if (::listen(listen_fd_, SOMAXCONN) < 0) {
            throw std::runtime_error("Failed to listen");
        }

        // 注册读事件到事件循环，当有新连接时触发回调
        channel_->enable_reading();
    }

    /**
     * @brief 处理新连接到达事件
     *
     * 当 listen_fd_ 可读时被调用，表示有新连接等待处理
     * 流程：accept() → 构建客户端地址 → 调用新连接回调
     */
    void Acceptor::handle_read() {
        // Loop accept() to drain all pending connections.
        // On Linux with edge-triggered epoll, a single event may signal
        // multiple queued connections; accepting only one per event risks
        // filling the backlog under high connection rates.
        static constexpr int kMaxAcceptPerEvent = 64;
        for (int round = 0; round < kMaxAcceptPerEvent; ++round) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            socket_t conn_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);

            if (conn_fd != INVALID_SOCKET_VAL) {
                char ip[INET_ADDRSTRLEN] = { 0 };
                ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
                uint16_t port = ntohs(addr.sin_port);
                std::string peer_addr = std::string(ip) + ":" + std::to_string(port);

                if (new_connection_callback_) {
                    new_connection_callback_(conn_fd, peer_addr);
                } else {
                    close_socket(conn_fd);
                }
                continue;
            }

            // No more pending connections or error.
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                break;
            if (round > 0 || err != WSAEWOULDBLOCK) {
                // Only log if it's not the first-round WSAEWOULDBLOCK.
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
#endif
            break;
        }
    }

} // namespace corodb
