// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file network.h @brief 客户端网络辅助接口。 */

#pragma once

#include <string>
#include "corodb/net/port.h"

namespace corodb {

    /** @brief 设置套接字为非阻塞模式。 */
    void set_nonblock(socket_t fd);

    /** @brief 建立到远端主机的 TCP 连接。 */
    socket_t connect_socket(const std::string& host, int port);

    /** @brief 判断响应是否表示服务端错误。 */
    bool is_error_response(const std::string& response) noexcept;

    /** @brief 等待服务端端口进入可连接状态。 */
    bool wait_for_server(const std::string& host, int port, int timeout_ms);

    /** @brief 发送一行文本协议数据。 */
    bool send_line(socket_t fd, const std::string& line);

    /** @brief 读取一条完整的服务端响应。 */
    std::string read_response(socket_t fd);

} // namespace corodb
