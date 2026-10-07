// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file replication.h @brief WAL 日志复制（P4）：复制记录格式 + 主端推送 + 从端应用。
 *
 * 架构（参考 PostgreSQL 流复制 / OceanBase Paxos 日志的简化异步形态）：
 *   - 主端 LSMTreeEngine 的每次变更（行写入/删除/提交标记/建表/删表）通过 sink 回调
 *     产出 ReplicationRecord，ReplicationHub 序列化后按连接 FIFO 推送给各从端；
 *   - 从端 ReplicationFollower 常驻接收，逐帧反序列化后经回调应用到本地引擎
 *     （append_row + mark_committed 重放主端 MVCC 状态）；
 *   - COMMIT 记录先于其行记录到达的乱序不会发生（同一 TCP 连接 FIFO），
 *     行记录已应用但 COMMIT 未到时数据不可见 —— 与主端崩溃恢复语义一致；
 *   - 从端必须先从主端 BACKUP 快照引导（数据目录拷贝）再接入流复制；
 *   - 从端为只读：查询直接服务本地数据，写语句被拒绝。
 *
 * 帧格式：[u32 payload_len][payload]。payload 首字节为记录类型。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "corodb/common/metrics.h"
#include "corodb/net/port.h"
#include "corodb/net/tls.h"
#include "corodb/storage/table.h"

namespace corodb {

    /** @brief 单条复制记录（主端一次变更的日志单元）。
     *
     * 行/键以引擎编码字节串承载（encode_row/encode_key），因为解码需要目标表的
     * 列定义 —— 该定义在从端按 CREATE_TABLE 记录或本地 load_schema 获得。 */
    struct ReplicationRecord {
        enum class Type : uint8_t {
            ApplyRow = 1,   ///< 写入/覆盖一行（table, commit_ts, row_wire = encode_row）
            ApplyDelete = 2, ///< 按主键删除（table, commit_ts, row_wire = encode_key）
            Commit = 3,      ///< 全局提交标记（commit_ts）
            CreateTable = 4, ///< 建表（table, columns）
            DropTable = 5,   ///< 删表（table）
        };

        Type type{ Type::ApplyRow };
        std::string table;            ///< 目标表（Commit 类型为空）
        uint64_t commit_ts{ 0 };      ///< 提交时间戳
        std::string row_wire;         ///< ApplyRow: encode_row 字节；ApplyDelete: encode_key 字节
        std::vector<Column> columns;  ///< CreateTable：列定义

        /** @brief 序列化为 payload 字节串。 */
        [[nodiscard]] std::string serialize() const;

        /** @brief 从 payload 反序列化；失败返回 false。 */
        bool deserialize(const std::string& payload);

        /** @brief 以 [u32 len][payload] 帧封装（主端发送）。 */
        [[nodiscard]] std::string frame() const;
    };

    /**
     * @brief 主端复制集线器：监听端口、管理从端连接、广播记录。
     *
     * broadcast() 由写入线程同步调用：序列化一次，逐连接入队（无界，背压靠
     * 应用层节奏；从端掉线自动剔除）。每连接一个 sender 线程按 FIFO 发送，
     * 保证行记录先于其 COMMIT 记录到达。
     */
    class ReplicationHub {
    public:
        ReplicationHub();
        ~ReplicationHub();

        ReplicationHub(const ReplicationHub&) = delete;
        ReplicationHub& operator=(const ReplicationHub&) = delete;

        /** @brief 监听端口并开始接受从端连接；绑定失败抛 std::runtime_error。 */
        void start(uint16_t port);

        /** @brief 停止监听并断开全部从端。 */
        void stop();

        /** @brief 实际监听端口（start 后有效；port 0 = OS 分配）。 */
        [[nodiscard]] uint16_t port() const noexcept {
            return port_.load();
        }

        /** @brief 当前连接的从端数量。 */
        [[nodiscard]] std::size_t follower_count() const;

        /** @brief 广播一条记录到所有在连从端。 */
        void broadcast(const ReplicationRecord& record);

        /** @brief 启用 TLS：accept 后先握手再推流（须在 start 前调用）。 */
        void set_tls(std::shared_ptr<tls::TlsContext> tls) {
            tls_ = std::move(tls);
        }

    private:
        struct Client {
            socket_t fd{ INVALID_SOCKET_VAL };
            std::queue<std::string> outbox; ///< 已封帧的待发数据
            std::mutex mutex;
            std::atomic<bool> alive{ true };
            std::shared_ptr<tls::TlsStream> stream; ///< TLS 已握手时使用
        };

        void accept_loop();
        void sender_loop(std::shared_ptr<Client> client);
        void reap_dead_clients();

        socket_t listen_fd_{ INVALID_SOCKET_VAL };
        std::atomic<uint16_t> port_{ 0 };
        std::atomic<bool> running_{ false };
        std::thread accept_thread_;
        mutable std::mutex clients_mutex_;
        std::vector<std::shared_ptr<Client>> clients_;
        std::shared_ptr<tls::TlsContext> tls_;

        Counter& sent_counter_;
        Gauge& connected_gauge_;
    };

    /**
     * @brief 从端复制接收器：连接主端，逐帧接收并应用记录。
     *
     * 断线自动重连（500ms 间隔）；每次成功应用计数并更新 applied_commit_ts 指标。
     */
    class ReplicationFollower {
    public:
        using ApplyFn = std::function<void(const ReplicationRecord&)>;

        ReplicationFollower();
        ~ReplicationFollower();

        ReplicationFollower(const ReplicationFollower&) = delete;
        ReplicationFollower& operator=(const ReplicationFollower&) = delete;

        /** @brief 启动接收线程（连接失败在后台按间隔重试）。 */
        void start(const std::string& host, uint16_t port, ApplyFn apply);

        /** @brief 停止接收线程。 */
        void stop();

        /** @brief 启用 TLS：连接成功后以客户端身份握手（须在 start 前调用）。 */
        void set_tls(std::shared_ptr<tls::TlsContext> tls) {
            tls_ = std::move(tls);
        }

        [[nodiscard]] bool connected() const noexcept {
            return connected_.load();
        }

    private:
        void receive_loop();
        bool try_connect();

        std::string host_;
        uint16_t port_{ 0 };
        ApplyFn apply_;
        std::shared_ptr<tls::TlsContext> tls_;
        std::shared_ptr<tls::TlsStream> stream_;
        socket_t fd_{ INVALID_SOCKET_VAL };
        std::atomic<bool> running_{ false };
        std::atomic<bool> connected_{ false };
        std::thread thread_;

        Counter& applied_counter_;
        Gauge& applied_commit_ts_;
    };

} // namespace corodb
