// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file reactor_server.h @brief Reactor 服务器对象定义。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "corodb/net/port.h"
#include "corodb/threading/acceptor.h"
#include "corodb/threading/connection.h"
#include "corodb/threading/event_loop.h"
#include "corodb/threading/thread_pool.h"

namespace corodb {

    /** @brief Reactor 服务器配置。 */
    struct ReactorServerConfig {
        uint16_t port{ 4000 };                ///< 监听端口
        std::size_t io_threads{ 0 };          ///< I/O 线程数，0 表示使用 CPU 核心数
        std::size_t worker_threads{ 0 };      ///< 工作线程数，0 表示使用 CPU 核心数
        std::size_t max_connections{ 10000 }; ///< 最大连接数
        bool reuse_port{ true };              ///< 是否复用端口
        std::chrono::seconds idle_timeout{ 0 }; ///< 空闲超时（0 = 禁用）
        std::size_t thread_pool_max_queue{ 0 }; ///< 线程池队列上限（0 = 无限制）
    };

    /** @brief Reactor 模式网络服务器，整合 EventLoop、Acceptor、Connection 和 ThreadPool。 */
    class ReactorServer {
    public:
        using MessageCallback = std::function<void(const ConnectionPtr&, std::string&)>;
        using ConnectionCallback = std::function<void(const ConnectionPtr&)>;

        /** @brief 构造服务器。 @param config 服务器配置。 */
        explicit ReactorServer(const ReactorServerConfig& config);

        ~ReactorServer();

        // 禁止复制
        ReactorServer(const ReactorServer&) = delete;
        ReactorServer& operator=(const ReactorServer&) = delete;

        /** @brief 设置消息回调（收到客户端消息时在 I/O 线程中调用）。 */
        void set_message_callback(MessageCallback cb) {
            message_callback_ = std::move(cb);
        }

        /** @brief 设置连接回调（连接建立时调用）。 */
        void set_connection_callback(ConnectionCallback cb) {
            connection_callback_ = std::move(cb);
        }

        /** @brief 设置关闭回调（连接关闭时调用，在从连接表移除前触发）。 */
        void set_close_callback(CloseCallback cb) {
            close_callback_ = std::move(cb);
        }

        /** @brief 启动服务器，阻塞直到 stop() 被调用。 */
        void start();

        /** @brief 停止服务器，线程安全。 */
        void stop();

        /** @brief 提交任务到工作线程池。 @return std::future 用于获取任务返回值。 */
        template<typename F, typename... Args>
        auto submit_task(F&& f, Args&&... args) {
            return worker_pool_->submit(std::forward<F>(f), std::forward<Args>(args)...);
        }

        /** @brief 提交无返回值任务到工作线程池（避免 packaged_task 开销）。 */
        void post_task(std::function<void()> f) {
            worker_pool_->post(std::move(f));
        }

        /** @brief 获取当前连接数。 */
        [[nodiscard]] std::size_t connection_count() const;

        /** @brief 检查并关闭所有空闲超时的连接。 */
        void check_idle_connections();

        /** @brief 检查服务器是否正在运行。 */
        [[nodiscard]] bool is_running() const noexcept {
            return running_.load();
        }

    private:
        void on_new_connection(socket_t fd, const std::string& peer_addr);
        void on_connection_close(const ConnectionPtr& conn);

        ReactorServerConfig config_;

        // Main Reactor
        std::unique_ptr<EventLoop> main_loop_;
        std::unique_ptr<Acceptor> acceptor_;

        // Sub Reactors (I/O 线程池)
        std::unique_ptr<EventLoopThreadPool> io_pool_;

        // Worker 线程池
        std::unique_ptr<ThreadPool> worker_pool_;

        // 连接管理
        mutable std::mutex conn_mutex_;
        std::unordered_map<socket_t, ConnectionPtr> connections_;

        // 状态
        std::atomic<bool> running_{ false };
        std::atomic<bool> started_{ false };

        // 回调
        MessageCallback message_callback_;
        ConnectionCallback connection_callback_;
        CloseCallback close_callback_;
    };

} // namespace corodb
