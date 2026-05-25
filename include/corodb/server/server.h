/** @file server.h @brief 服务器配置与启动接口。 */

#pragma once

#include <cstdint>
#include <string>

namespace corodb {

    /** @brief 服务器启动配置（从 Config 和 corodb.conf 填充）。 */
    struct ServerConfig {
        uint16_t port{ 4000 };                ///< 监听端口。
        std::string data_dir{ "data" };       ///< 数据目录。
        std::size_t max_connections{ 10000 }; ///< 最大连接数。
        std::size_t io_threads{ 0 };          ///< I/O 线程数（0=自动）。
        std::size_t worker_threads{ 0 };      ///< 工作线程数（0=自动）。
        bool reuse_port{ true };              ///< SO_REUSEPORT。
        uint64_t idle_timeout_sec{ 0 };       ///< 空闲超时（秒，0=禁用）。
        uint64_t statement_timeout_ms{ 0 };   ///< 语句超时（毫秒，0=禁用）。
        std::size_t thread_pool_max_queue{ 0 }; ///< 线程池队列上限（0=无限制）。
    };

    /**
     * @brief 启动 Reactor 模式 SQL 服务器，阻塞直到服务器停止。
     * @return 0 表示正常退出，非零表示错误。
     */
    int run_server(const ServerConfig& cfg);

    /** @brief 请求服务器优雅停止（线程安全）。 */
    void request_server_shutdown();

    /** @brief 返回服务器当前是否处于运行状态。 */
    [[nodiscard]] bool is_server_running();

} // namespace corodb
