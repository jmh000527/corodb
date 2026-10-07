// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file conn_pool.cpp
// @brief 客户端读写分离连接池实现。

#include "corodb/net/conn_pool.h"

#include <cctype>

namespace corodb {

    bool ClientConnPool::routes_to_replica(const std::string& sql) const {
        if (!replica_.has_value())
            return false;
        // 大写化首词。
        std::string head;
        for (char c : sql) {
            if (std::isspace(static_cast<unsigned char>(c)))
                continue;
            head.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            if (head.size() >= 5)
                break;
        }
        return head == "SELEC" || head == "SHOW";
    }

    socket_t ClientConnPool::ensure(socket_t& fd, const std::string& host, int port) {
        if (fd != INVALID_SOCKET_VAL)
            return fd;
        fd = connect_socket(host, port);
        return fd;
    }

    socket_t ClientConnPool::get(bool replica, std::string& error) {
        if (replica && replica_.has_value()) {
            socket_t fd = ensure(replica_fd_, replica_->first, replica_->second);
            if (fd == INVALID_SOCKET_VAL)
                error = "cannot connect to replica " + replica_->first + ":" + std::to_string(replica_->second);
            return fd;
        }
        socket_t fd = ensure(primary_fd_, host_, port_);
        if (fd == INVALID_SOCKET_VAL)
            error = "cannot connect to primary " + host_ + ":" + std::to_string(port_);
        return fd;
    }

    std::string ClientConnPool::execute(const std::string& sql) {
        const bool to_replica = routes_to_replica(sql);
        std::string error;
        socket_t fd = get(to_replica, error);
        if (fd == INVALID_SOCKET_VAL) {
            // 重连一次（对端可能重启过）。
            if (to_replica) {
                close_socket(replica_fd_);
                replica_fd_ = INVALID_SOCKET_VAL;
            } else {
                close_socket(primary_fd_);
                primary_fd_ = INVALID_SOCKET_VAL;
            }
            fd = get(to_replica, error);
            if (fd == INVALID_SOCKET_VAL)
                return "ERROR: " + error + "\n";
        }
        if (!send_line(fd, sql))
            return "ERROR: failed to send SQL\n";
        return read_response(fd);
    }

    void ClientConnPool::close_all() {
        if (primary_fd_ != INVALID_SOCKET_VAL) {
            close_socket(primary_fd_);
            primary_fd_ = INVALID_SOCKET_VAL;
        }
        if (replica_fd_ != INVALID_SOCKET_VAL) {
            close_socket(replica_fd_);
            replica_fd_ = INVALID_SOCKET_VAL;
        }
    }

} // namespace corodb
