// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file reactor_server.cpp
// @brief Reactor 模式 TCP 服务器的实现。

#include "corodb/threading/reactor_server.h"

#include <iostream>

#include "corodb/common/logger.h"

namespace corodb {

    // ============================================================================
    //                          ReactorServer 构造与析构
    // ============================================================================

    /**
     * @brief ReactorServer 构造函数
     * @param config 服务器配置
     *
     * 初始化多 Reactor 多线程服务器架构：
     * - Main Reactor：在主线程中接受新连接
     * - Sub Reactors：在 I/O 线程池中处理读写事件
     * - Worker Pool：处理计算密集型任务（如 SQL 执行）
     */
    ReactorServer::ReactorServer(const ReactorServerConfig& config) : config_(config) {

        // ==== 确定线程数量 ====
        std::size_t io_threads = config.io_threads;
        std::size_t worker_threads = config.worker_threads;

        // 如果未指定，使用 CPU 核心数
        if (io_threads == 0) {
            io_threads = std::thread::hardware_concurrency();
            if (io_threads == 0)
                io_threads = 4; // 安全默认值
        }

        if (worker_threads == 0) {
            worker_threads = std::thread::hardware_concurrency();
            if (worker_threads == 0)
                worker_threads = 4;
        }

        // ==== 创建 Main Reactor ====
        // Main Reactor 运行在主线程，专门处理新连接
        main_loop_ = std::make_unique<EventLoop>();

        // ==== 创建 Acceptor ====
        // Acceptor 负责监听端口并接受新连接
        acceptor_ = std::make_unique<Acceptor>(main_loop_.get(), config.port, config.reuse_port);
        acceptor_->set_new_connection_callback(
                [this](socket_t fd, const std::string& peer_addr) { on_new_connection(fd, peer_addr); });

        // ==== 创建 I/O 线程池 ====
        // 每个 I/O 线程运行一个 Sub-Reactor
        io_pool_ = std::make_unique<EventLoopThreadPool>(main_loop_.get(), io_threads);

        // ==== 创建工作线程池 ====
        // 用于执行计算密集型任务（如 SQL 解析执行）
        worker_pool_ = std::make_unique<ThreadPool>(worker_threads);
        if (config.thread_pool_max_queue > 0)
            worker_pool_->set_max_queue_size(config.thread_pool_max_queue);

        LOG_INFO("Reactor server configured: port={} io_threads={} workers={} max_connections={}",
                 config.port, io_threads, worker_threads, config.max_connections);
    }

    ReactorServer::~ReactorServer() {
        stop();
    }

    // ============================================================================
    //                          服务器启动与停止
    // ============================================================================

    /**
     * @brief 启动服务器
     *
     * 此函数会阻塞直到服务器停止
     * 启动顺序：
     * 1. 启动 I/O 线程池（Sub-Reactors）
     * 2. 开始监听端口
     * 3. 运行 Main Reactor 事件循环
     */
    void ReactorServer::start() {
        // 防止重复启动
        if (started_.exchange(true)) {
            return;
        }

        running_.store(true);

        // 启动 I/O 线程池（创建 Sub-Reactor 线程）
        io_pool_->start();

        // 开始监听新连接
        acceptor_->listen();

        LOG_INFO("Reactor server listening on port {}", config_.port);

        // 运行 Main Reactor（阻塞，直到 quit() 被调用）
        main_loop_->loop();

        running_.store(false);
        LOG_INFO("Reactor server stopped");
    }

    /**
     * @brief 停止服务器
     *
     * 优雅关闭顺序：
     * 1. 关闭所有客户端连接
     * 2. 停止 I/O 线程池
     * 3. 停止 Main Reactor
     */
    void ReactorServer::stop() {
        if (!running_.load()) {
            return;
        }

        running_.store(false);

        // ==== 关闭所有连接 ====
        {
            std::lock_guard lock(conn_mutex_);
            for (auto& [fd, conn]: connections_) {
                // 在各自的 I/O 线程中关闭连接
                conn->get_loop()->run_in_loop([conn]() { conn->force_close(); });
            }
            connections_.clear();
        }

        // ==== 停止 I/O 线程池 ====
        io_pool_->stop();

        // ==== 停止 Main Reactor ====
        main_loop_->quit();
    }

    /**
     * @brief 获取当前连接数
     * @return 活跃连接数量
     */
    std::size_t ReactorServer::connection_count() const {
        std::lock_guard lock(conn_mutex_);
        return connections_.size();
    }

    // ============================================================================
    //                          连接管理
    // ============================================================================

    /**
     * @brief 处理新连接
     * @param fd 新连接的 socket 描述符
     * @param peer_addr 客户端地址（"IP:Port"）
     *
     * 此回调由 Acceptor 在 Main Reactor 线程中调用
     * 处理流程：
     * 1. 检查连接数限制
     * 2. 设置非阻塞模式
     * 3. 选择 Sub-Reactor（Round-Robin）
     * 4. 创建 Connection 对象
     * 5. 在 I/O 线程中建立连接
     */
    void ReactorServer::on_new_connection(socket_t fd, const std::string& peer_addr) {
        // ==== 检查连接数限制 ====
        {
            std::lock_guard lock(conn_mutex_);
            if (connections_.size() >= config_.max_connections) {
                LOG_INFO("Max connections reached, rejecting {}", peer_addr);
                close_socket(fd); // 拒绝连接
                return;
            }
        }

        // ==== 设置非阻塞模式 ====
        // 新 socket 默认是阻塞的，需要设置为非阻塞
        set_nonblocking(fd);

        // ==== 选择 Sub-Reactor ====
        // 使用 Round-Robin 策略将连接分配到 I/O 线程
        EventLoop* io_loop = io_pool_->get_next_loop();

        // ==== 创建 Connection 对象 ====
        auto conn = std::make_shared<Connection>(io_loop, fd, peer_addr);

        // ==== 设置回调 ====
        if (config_.idle_timeout.count() > 0) {
            conn->set_idle_timeout(config_.idle_timeout);
        }
        conn->set_message_callback(message_callback_);       // 数据到达回调
        conn->set_connection_callback(connection_callback_); // 连接状态回调
        conn->set_close_callback([this](const ConnectionPtr& c) {
            on_connection_close(c); // 连接关闭回调
        });

        // ==== 添加到连接表 ====
        {
            std::lock_guard lock(conn_mutex_);
            connections_[fd] = conn;
        }

        // std::println("New connection from {} (total: {})", peer_addr, connection_count());

        // ==== 在 I/O 线程中建立连接 ====
        // 将任务投递到目标 Sub-Reactor，避免跨线程操作
        io_loop->run_in_loop([conn]() { conn->connection_established(); });
    }

    /**
     * @brief 处理连接关闭
     * @param conn 关闭的连接
     *
     * 此回调由 Connection 在其 I/O 线程中调用
     * 处理流程：
     * 1. 从连接表中移除（在 Main Reactor 线程）
     * 2. 销毁连接对象（在 I/O 线程）
     */
    void ReactorServer::on_connection_close(const ConnectionPtr& conn) {
        // Allow the application layer to clean up per-connection resources
        // (e.g., rollback active transactions, release locks).
        if (close_callback_) {
            close_callback_(conn);
        }

        // ==== 在 Main Reactor 线程中从连接表移除 ====
        // 因为 connections_ 在 Main Reactor 线程中管理
        main_loop_->run_in_loop([this, conn]() {
            {
                std::lock_guard lock(conn_mutex_);
                // 遍历查找并移除（而不是用 fd 查找，因为 fd 可能已被重用）
                for (auto it = connections_.begin(); it != connections_.end();) {
                    if (it->second == conn) {
                        it = connections_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        });

        // ==== 在 I/O 线程中销毁连接 ====
        conn->get_loop()->run_in_loop([conn]() { conn->connection_destroyed(); });
    }

    void ReactorServer::check_idle_connections() {
        if (config_.idle_timeout.count() <= 0)
            return;
        std::lock_guard lock(conn_mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (it->second->is_idle_timeout()) {
                auto conn = it->second;
                it = connections_.erase(it);
                conn->get_loop()->run_in_loop([conn]() { conn->force_close(); });
            } else {
                ++it;
            }
        }
    }

} // namespace corodb
