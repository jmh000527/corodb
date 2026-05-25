// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file network.cpp
// @brief 跨平台网络工具函数的实现。

#include "corodb/net/network.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace corodb {

    // ============================================================================
    //                          Socket 配置函数
    // ============================================================================

    void set_nonblock(socket_t fd) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    // ============================================================================
    //                          错误判定 / 服务器探测
    // ============================================================================

    bool is_error_response(const std::string& response) noexcept {
        std::size_t i = 0;
        while (i < response.size() && std::isspace(static_cast<unsigned char>(response[i]))) {
            ++i;
        }
        constexpr const char* kPrefix = "ERROR";
        constexpr std::size_t kLen = 5;
        if (response.size() - i < kLen)
            return false;
        for (std::size_t k = 0; k < kLen; ++k) {
            unsigned char a = static_cast<unsigned char>(response[i + k]);
            unsigned char b = static_cast<unsigned char>(kPrefix[k]);
            if (std::toupper(a) != b)
                return false;
        }
        return true;
    }

    bool wait_for_server(const std::string& host, int port, int timeout_ms) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            socket_t fd = connect_socket(host, port);
            if (fd != INVALID_SOCKET_VAL) {
                close_socket(fd);
                return true;
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeout_ms)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // ============================================================================
    //                          连接管理函数
    // ============================================================================

    /**
     * @brief 连接到指定主机和端口
     * @param host 主机名或 IP 地址
     * @param port 端口号
     * @return 成功返回 socket 描述符，失败返回 INVALID_SOCKET_VAL
     */
    socket_t connect_socket(const std::string& host, int port) {
        // 准备地址解析参数
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;     // 支持 IPv4 和 IPv6
        hints.ai_socktype = SOCK_STREAM; // TCP 流套接字
        auto port_str = std::to_string(port);

        // 解析主机地址
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
            return INVALID_SOCKET_VAL;

        socket_t fd = INVALID_SOCKET_VAL;

        // 遍历地址列表，尝试连接
        for (addrinfo* p = res; p; p = p->ai_next) {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd == INVALID_SOCKET_VAL)
                continue;
            if (::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0)
                break; // 连接成功
            close_socket(fd);
            fd = INVALID_SOCKET_VAL;
        }
        ::freeaddrinfo(res);
        return fd;
    }

    // ============================================================================
    //                          数据发送接收函数
    // ============================================================================

    /**
     * @brief 发送一行数据到 socket
     * @param fd Socket 文件描述符
     * @param line 要发送的数据行
     * @return 成功返回 true，失败返回 false
     *
     * 如果数据不以换行符结尾，会自动追加换行符。
     * 在非阻塞模式下会自动重试直到发送完成。
     */
    bool send_line(socket_t fd, const std::string& line) {
        // 确保数据以换行符结尾
        std::string data = line;
        if (data.empty() || data.back() != '\n')
            data.push_back('\n');

        constexpr int kSendTimeoutMs = 30000; // 总写入超时
        auto start = std::chrono::steady_clock::now();

        std::size_t sent = 0;

        // 循环发送直到所有数据发送完成
        while (sent < data.size()) {
            int n = write_socket(fd, data.data() + sent, data.size() - sent);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }

            // 检查是否是非阻塞模式的暂时不可用
            bool would_block = false;
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                would_block = true;
#else
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                would_block = true;
#endif

            if (!would_block) {
                return false; // 真正的发送错误
            }

            // 等待 socket 可写，避免在 EWOULDBLOCK 上忙等自旋。
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= kSendTimeoutMs)
                return false;
            int remaining = kSendTimeoutMs - static_cast<int>(elapsed);

#ifdef _WIN32
            WSAPOLLFD pfd{};
            pfd.fd = fd;
            pfd.events = POLLWRNORM;
            int poll_ret = ::WSAPoll(&pfd, 1, remaining);
#else
            pollfd pfd{ fd, POLLOUT, 0 };
            int poll_ret = ::poll(&pfd, 1, remaining);
#endif
            if (poll_ret <= 0)
                return false; // 超时或错误
            // 可写后回到循环顶端再次尝试 write
        }
        return true;
    }

    /**
     * @brief 从 socket 读取响应数据
     * @param fd Socket 文件描述符
     * @return 读取到的响应字符串
     *
     * 使用 @END 标记检测响应结束。
     * 采用业界主流的单次 poll 超时模式，避免循环轮询开销。
     */
    std::string read_response(socket_t fd) {
        std::string out;
        char buf[4096]; // 4KB 缓冲区

        // 响应终止标记常量
        constexpr const char* kEndMarker = "@END\n";
        constexpr size_t kEndMarkerLen = 5;
        constexpr int kTimeoutMs = 30000; // 30 秒总超时（支持大批量操作）

        auto start = std::chrono::steady_clock::now();

        for (;;) {
            // 检查总超时
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= kTimeoutMs)
                break;

            int remaining = kTimeoutMs - static_cast<int>(elapsed);

            // 单次 poll 等待数据（业界主流做法：一次 poll 等待剩余超时时间）
#ifdef _WIN32
            WSAPOLLFD pfd{};
            pfd.fd = fd;
            pfd.events = POLLRDNORM;
            int poll_ret = ::WSAPoll(&pfd, 1, remaining);
#else
            pollfd pfd{ fd, POLLIN, 0 };
            int poll_ret = ::poll(&pfd, 1, remaining);
#endif

            if (poll_ret <= 0)
                break; // 超时或错误

            // 有数据可读
            int n = read_socket(fd, buf, sizeof(buf));

            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));

                // 检查是否收到终止标记
                if (out.size() >= kEndMarkerLen &&
                    out.compare(out.size() - kEndMarkerLen, kEndMarkerLen, kEndMarker) == 0) {
                    out.erase(out.size() - kEndMarkerLen);
                    break;
                }
                continue;
            }

            if (n == 0)
                break; // 连接关闭

            // 检查是否需要重试（非阻塞模式）
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                break;
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                break;
#endif
        }
        return out;
    }

} // namespace corodb
