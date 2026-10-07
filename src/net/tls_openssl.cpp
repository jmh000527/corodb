// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file tls_openssl.cpp
// @brief TLS 传输加密的 OpenSSL 后端（全平台；P2）。
//
// 服务器证书：[tls].cert_path（PEM 证书链）+ [tls].key_path（PEM 私钥）。
// 客户端：verify_cert=true 启用系统根验证（可加 ca_path 信任库）；
//         false 设置 SSL_VERIFY_NONE 接受自签名服务器（测试/TOFU）。
// 要求传入 TlsStream 的 socket 为阻塞模式。

#include "corodb/net/tls.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>

namespace corodb::tls {

    namespace {

        std::string openssl_errors() {
            std::string out;
            unsigned long e = ERR_get_error();
            char buf[256];
            while (e != 0) {
                ERR_error_string_n(e, buf, sizeof(buf));
                if (!out.empty())
                    out += "; ";
                out += buf;
                e = ERR_get_error();
            }
            return out;
        }

        void raw_send_all(socket_t fd, const char* data, std::size_t n) {
            std::size_t sent = 0;
            while (sent < n) {
                const int k = write_socket(fd, data + sent, n - sent);
                if (k <= 0)
                    throw TlsError("[TLS] socket send failed");
                sent += static_cast<std::size_t>(k);
            }
        }

        std::size_t raw_recv_some(socket_t fd, char* data, std::size_t n) {
            for (;;) {
                const int k = read_socket(fd, data, static_cast<int>(n));
                if (k > 0)
                    return static_cast<std::size_t>(k);
                if (k == 0)
                    return 0;
                throw TlsError("[TLS] socket recv failed");
            }
        }

        struct CtxState {
            SSL_CTX* ctx{ nullptr };
        };

        void destroy_ctx_state(void* p) noexcept {
            auto* st = static_cast<CtxState*>(p);
            if (st->ctx)
                SSL_CTX_free(st->ctx);
            delete st;
        }

    } // namespace

    void TlsContext::Deleter::operator()(void* p) const noexcept {
        if (fn && p)
            reinterpret_cast<void (*)(void*)>(fn)(p);
    }

    std::shared_ptr<TlsContext> TlsContext::create_server(const TlsConfig& cfg) {
        auto* st = new CtxState();
        st->ctx = SSL_CTX_new(TLS_server_method());
        if (!st->ctx) {
            delete st;
            throw TlsError("[TLS] SSL_CTX_new failed: " + openssl_errors());
        }
        SSL_CTX_set_min_proto_version(st->ctx, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_chain_file(st->ctx, cfg.cert_path.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(st->ctx, cfg.key_path.empty() ? cfg.cert_path.c_str() : cfg.key_path.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(st->ctx) != 1) {
            const std::string err = openssl_errors();
            SSL_CTX_free(st->ctx);
            delete st;
            throw TlsError("[TLS] loading server certificate/key failed: " + err);
        }
        auto ctx = std::shared_ptr<TlsContext>(new TlsContext());
        // native_handle 直接持有 SSL_CTX*（省一层 CtxState 间接）。
        ctx->handle_.reset(st->ctx);
        ctx->handle_.get_deleter().fn = reinterpret_cast<void*>(&SSL_CTX_free);
        delete st;
        return ctx;
    }

    std::shared_ptr<TlsContext> TlsContext::create_client(const TlsConfig& cfg) {
        auto* st = new CtxState();
        st->ctx = SSL_CTX_new(TLS_client_method());
        if (!st->ctx) {
            delete st;
            throw TlsError("[TLS] SSL_CTX_new failed: " + openssl_errors());
        }
        SSL_CTX_set_min_proto_version(st->ctx, TLS1_2_VERSION);
        if (cfg.verify_cert) {
            SSL_CTX_set_verify(st->ctx, SSL_VERIFY_PEER, nullptr);
            // 系统根库加载失败不致命（OpenSSL 3 在无 OPENSSLDIR 的发行上可能返回 0）；
            // 显式 ca_path 是可靠路径。
            if (!cfg.ca_path.empty())
                SSL_CTX_load_verify_locations(st->ctx, cfg.ca_path.c_str(), nullptr);
            else
                SSL_CTX_set_default_verify_paths(st->ctx);
        } else {
            SSL_CTX_set_verify(st->ctx, SSL_VERIFY_NONE, nullptr);
        }
        auto ctx = std::shared_ptr<TlsContext>(new TlsContext());
        ctx->handle_.reset(st->ctx);
        ctx->handle_.get_deleter().fn = reinterpret_cast<void*>(&SSL_CTX_free);
        delete st;
        return ctx;
    }

    TlsContext::~TlsContext() = default;

    // =========================================================================
    // TlsStream
    // =========================================================================

    struct TlsStream::Impl {
        std::shared_ptr<TlsContext> ctx;
        SSL* ssl{ nullptr };
        bool shutdown_done{ false };
    };

    std::unique_ptr<TlsStream> TlsStream::accept(std::shared_ptr<TlsContext> ctx, socket_t fd) {
        auto stream = std::unique_ptr<TlsStream>(new TlsStream());
        stream->impl_ = std::make_unique<Impl>();
        Impl& p = *stream->impl_;
        p.ctx = std::move(ctx);
        p.ssl = SSL_new(static_cast<SSL_CTX*>(p.ctx->native_handle()));
        if (!p.ssl)
            throw TlsError("[TLS] SSL_new failed");
        if (SSL_set_fd(p.ssl, static_cast<int>(fd)) != 1) {
            SSL_free(p.ssl);
            throw TlsError("[TLS] SSL_set_fd failed");
        }
        if (SSL_accept(p.ssl) != 1) {
            const std::string err = openssl_errors();
            SSL_free(p.ssl);
            throw TlsError("[TLS] SSL_accept failed: " + err);
        }
        return stream;
    }

    std::unique_ptr<TlsStream> TlsStream::connect(std::shared_ptr<TlsContext> ctx, socket_t fd,
                                                  const std::string& host) {
        auto stream = std::unique_ptr<TlsStream>(new TlsStream());
        stream->impl_ = std::make_unique<Impl>();
        Impl& p = *stream->impl_;
        p.ctx = std::move(ctx);
        p.ssl = SSL_new(static_cast<SSL_CTX*>(p.ctx->native_handle()));
        if (!p.ssl)
            throw TlsError("[TLS] SSL_new failed");
        SSL_set_fd(p.ssl, static_cast<int>(fd));
        SSL_set_tlsext_host_name(p.ssl, (host.empty() ? "localhost" : host).c_str());
        if (SSL_connect(p.ssl) != 1) {
            const std::string err = openssl_errors();
            SSL_free(p.ssl);
            throw TlsError("[TLS] SSL_connect failed: " + err);
        }
        return stream;
    }

    TlsStream::~TlsStream() {
        if (impl_ && impl_->ssl)
            SSL_free(impl_->ssl);
    }

    std::size_t TlsStream::read(char* buf, std::size_t len) {
        Impl& p = *impl_;
        for (;;) {
            const int n = SSL_read(p.ssl, buf, static_cast<int>(len));
            if (n > 0)
                return static_cast<std::size_t>(n);
            const int err = SSL_get_error(p.ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN)
                return 0;
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue; // 阻塞 socket 上不应出现，保险重试
            if (err == SSL_ERROR_SYSCALL && errno == EINTR)
                continue;
            throw TlsError("[TLS] SSL_read failed: " + openssl_errors());
        }
    }

    void TlsStream::write(const char* data, std::size_t len) {
        Impl& p = *impl_;
        std::size_t off = 0;
        while (off < len) {
            const int n = SSL_write(p.ssl, data + off, static_cast<int>(len - off));
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            const int err = SSL_get_error(p.ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            if (err == SSL_ERROR_SYSCALL && errno == EINTR)
                continue;
            throw TlsError("[TLS] SSL_write failed: " + openssl_errors());
        }
    }

    void TlsStream::shutdown() {
        Impl& p = *impl_;
        if (p.shutdown_done || !p.ssl)
            return;
        p.shutdown_done = true;
        SSL_shutdown(p.ssl);
    }

} // namespace corodb::tls
