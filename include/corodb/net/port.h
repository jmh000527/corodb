// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file port.h @brief 跨平台套接字与 I/O 兼容层。 */

#pragma once
#include <cstdint>

#ifdef _WIN32
// ---- Windows 平台实现 ----

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/** @brief 跨平台套接字类型（Windows: SOCKET，Linux: int）。 */
using socket_t = SOCKET;

/** @brief 无效套接字值（用于检测套接字操作失败）。 */
#define INVALID_SOCKET_VAL INVALID_SOCKET

/** @brief 套接字操作失败时的返回值。 */
#define SOCKET_ERROR_VAL SOCKET_ERROR

/** @brief 关闭套接字（Windows: closesocket，Linux: close）。 */
#define close_socket closesocket

/** @brief 向套接字写入数据（Windows: send，Linux: write）。 */
#define write_socket(fd, buf, len) ::send(fd, buf, static_cast<int>(len), 0)

/** @brief 从套接字读取数据（Windows: recv，Linux: read）。 */
#define read_socket(fd, buf, len) ::recv(fd, buf, static_cast<int>(len), 0)

/** @brief 将套接字设为非阻塞模式。 */
inline void set_nonblocking(socket_t fd) {
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
}

// WSAPoll - MinGW或旧版SDK可能需要额外定义，但通常在winsock2.h中已包含

#include <fcntl.h> ///< 标准文件I/O控制

#else
// ---- Linux/Unix 平台实现 ----

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief 跨平台套接字类型（Windows: SOCKET，Linux: int）。 */
using socket_t = int;

/** @brief 无效套接字值（用于检测套接字操作失败）。 */
#define INVALID_SOCKET_VAL (-1)

/** @brief 套接字操作失败时的返回值。 */
#define SOCKET_ERROR_VAL (-1)

/** @brief 关闭套接字（Windows: closesocket，Linux: close）。 */
#define close_socket ::close

/** @brief 向套接字写入数据（Windows: send，Linux: write）。 */
#define write_socket(fd, buf, len) ::write(fd, buf, len)

/** @brief 从套接字读取数据（Windows: recv，Linux: read）。 */
#define read_socket(fd, buf, len) ::read(fd, buf, len)

/** @brief 将套接字设为非阻塞模式。 */
inline void set_nonblocking(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

// ---- 跨平台 I/O 事件常量 ----

namespace corodb {

    /**
     * @defgroup io_events I/O 事件常量
     * @brief 用于事件调度器的统一 I/O 事件抽象
     * @{
     */

#ifdef _WIN32
    /// 读就绪事件（Windows WSAPoll）。
    constexpr uint32_t EVENT_READ = POLLRDNORM;

    /// 写就绪事件（Windows WSAPoll）。
    constexpr uint32_t EVENT_WRITE = POLLWRNORM;
#else
    /// 读就绪事件（Linux Epoll）。
    constexpr uint32_t EVENT_READ = EPOLLIN;

    /// 写就绪事件（Linux Epoll）。
    constexpr uint32_t EVENT_WRITE = EPOLLOUT;
#endif

    /** @} */ // end of io_events group

} // namespace corodb
