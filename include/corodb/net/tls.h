// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file tls.h @brief TLS 传输加密抽象（P2）。
 *
 * 平台后端：Windows = 系统 Schannel/SSPI（secur32）；Linux/Unix = OpenSSL。
 * 抽象为两类对象：
 *   - TlsContext：按角色（服务器持证书 / 客户端持信任策略）构建的平台凭据；
 *   - TlsStream ：在已建立的（阻塞）socket 上完成握手并提供加密读写。
 *
 * 接入点：管理端点（HTTPS）、复制通道（Hub/Follower）、SQL TLS 专用监听端口
 * （[tls].sql_port，线程/连接，复用 SQL 文本协议）。要求传入的 socket 为阻塞模式。
 *
 * 服务器证书来源：
 *   - 生产：[tls].cert_path —— Windows 为 .pfx（PKCS#12，私钥内含）；
 *     Linux 为 PEM 证书（[tls].key_path 为 PEM 私钥）；
 *   - 开发/测试：cert_path 为空时生成运行时自签名证书（仅 Schannel 后端支持）。 */

#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "corodb/net/port.h"

namespace corodb::tls {

    struct TlsConfig {
        std::string cert_path;   ///< 服务器证书（Windows .pfx / Linux PEM）
        std::string key_path;    ///< 私钥（Linux PEM；Windows PFX 内含可留空）
        std::string pfx_password; ///< Windows PFX 导入密码（可空）
        std::string ca_path;     ///< 信任 CA（可选）
        bool verify_cert{ true }; ///< 客户端是否校验服务器证书（自签名测试场景置 false）
    };

    /** @brief TLS 握手失败/IO 错误。 */
    class TlsError : public std::runtime_error {
    public:
        explicit TlsError(const std::string& msg) : std::runtime_error(msg) {
        }
    };

    class TlsStream;

    /** @brief 平台 TLS 凭据上下文（不可变；线程安全）。 */
    class TlsContext {
    public:
        /** @brief 服务器上下文：cert_path 为空 → 运行时自签名证书（仅 Schannel 后端）。 */
        [[nodiscard]] static std::shared_ptr<TlsContext> create_server(const TlsConfig& cfg);
        /** @brief 客户端上下文：verify_cert=false 时接受自签名服务器（手动信任）。 */
        [[nodiscard]] static std::shared_ptr<TlsContext> create_client(const TlsConfig& cfg);
        ~TlsContext();
        TlsContext(const TlsContext&) = delete;
        TlsContext& operator=(const TlsContext&) = delete;

        /** @brief 平台句柄（Schannel: CredHandle*；OpenSSL: SSL_CTX*）。 */
        [[nodiscard]] void* native_handle() const noexcept {
            return handle_.get();
        }

    private:
        TlsContext() = default;
        struct Deleter {
            void* fn{ nullptr }; ///< 平台释放函数指针（避免后端头进入公共头）
            void operator()(void* p) const noexcept;
        };
        std::unique_ptr<void, Deleter> handle_;
        friend class TlsStream;
    };

    /** @brief 已握手 TLS 流（阻塞 socket 之上的加密读写）。 */
    class TlsStream {
    public:
        /** @brief 服务器侧握手（socket 须为阻塞模式）；失败抛 TlsError。 */
        [[nodiscard]] static std::unique_ptr<TlsStream> accept(std::shared_ptr<TlsContext> ctx, socket_t fd);
        /** @brief 客户端侧握手；失败抛 TlsError。 */
        [[nodiscard]] static std::unique_ptr<TlsStream> connect(std::shared_ptr<TlsContext> ctx, socket_t fd,
                                                                const std::string& host);
        ~TlsStream();
        TlsStream(const TlsStream&) = delete;
        TlsStream& operator=(const TlsStream&) = delete;

        /** @brief 解密读取：返回 0 表示对端有序关闭；至少读到 1 字节才返回。 */
        [[nodiscard]] std::size_t read(char* buf, std::size_t len);
        /** @brief 加密写入：完整写出全部数据。 */
        void write(const char* data, std::size_t len);
        void write(const std::string& data) {
            write(data.data(), data.size());
        }
        /** @brief 发送 close_notify 并释放平台资源（不关闭底层 fd）。 */
        void shutdown();

    private:
        TlsStream() = default;

        /** @brief 握手公共骨架（后端实现；accept/connect 调用）。 */
        static std::unique_ptr<TlsStream> handshake(std::shared_ptr<TlsContext> ctx, socket_t fd, bool client,
                                                    const std::string& host);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace corodb::tls
