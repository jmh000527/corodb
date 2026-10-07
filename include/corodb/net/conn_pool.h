// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file conn_pool.h @brief 客户端读写分离连接池（P4）。
 *
 * csql 客户端用：主端（写 + 默认读）与可选只读副本两条持久连接。
 * SELECT 显式路由到副本（--read-replica host:port 启用）；写语句与 DDL 恒走主端。
 * 断线时惰性重连。线程模型：单线程交互（与 csql 一致），无锁。 */

#pragma once

#include <optional>
#include <string>

#include "corodb/net/network.h"
#include "corodb/net/port.h"

namespace corodb {

    class ClientConnPool {
    public:
        /** @brief replica 为 nullopt 时退化为单连接（全部走主端）。 */
        explicit ClientConnPool(const std::string& primary_host, int primary_port,
                                std::optional<std::pair<std::string, int>> replica = std::nullopt)
            : host_(primary_host), port_(primary_port), replica_(std::move(replica)) {
        }

        ~ClientConnPool() {
            close_all();
        }

        ClientConnPool(const ClientConnPool&) = delete;
        ClientConnPool& operator=(const ClientConnPool&) = delete;

        /** @brief 语句是否应路由到只读副本（仅 SELECT/SHOW；显式开启副本时）。 */
        [[nodiscard]] bool routes_to_replica(const std::string& sql) const;

        /** @brief 取目标连接（惰性建连；失败返回 INVALID_SOCKET_VAL 并填 error）。 */
        [[nodiscard]] socket_t get(bool replica, std::string& error);

        /** @brief 经池执行一条 SQL，返回响应文本；失败返回 error 前缀标记。 */
        [[nodiscard]] std::string execute(const std::string& sql);

        [[nodiscard]] bool has_replica() const noexcept {
            return replica_.has_value();
        }

        void close_all();

    private:
        /** @brief 确保单条连接可用（重连一次）；失败返回 INVALID_SOCKET_VAL。 */
        static socket_t ensure(socket_t& fd, const std::string& host, int port);

        std::string host_;
        int port_;
        std::optional<std::pair<std::string, int>> replica_;
        socket_t primary_fd_{ INVALID_SOCKET_VAL };
        socket_t replica_fd_{ INVALID_SOCKET_VAL };
    };

} // namespace corodb
