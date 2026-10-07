// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file tls_sql_server.cpp
// @brief SQL over TLS 监听器实现（线程/连接阻塞模型）。

#include "corodb/server/tls_sql_server.h"

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "corodb/common/config.h"
#include "corodb/server/sql_protocol.h"

namespace corodb {

    TlsSqlServer::TlsSqlServer(uint16_t port, std::shared_ptr<tls::TlsContext> tls, Database& db)
        : port_(port), tls_(std::move(tls)), db_(db) {
    }

    TlsSqlServer::~TlsSqlServer() {
        stop();
    }

    void TlsSqlServer::start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET_VAL)
            throw std::runtime_error("[TlsSql] socket() failed");
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[TlsSql] bind() failed on port " + std::to_string(port_));
        }
        if (::listen(listen_fd_, 16) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[TlsSql] listen() failed");
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
            bound_port_.store(ntohs(bound.sin_port));
        running_.store(true);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    void TlsSqlServer::stop() {
        if (!running_.exchange(false))
            return;
        if (listen_fd_ != INVALID_SOCKET_VAL) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
        }
        if (accept_thread_.joinable())
            accept_thread_.join();
    }

    void TlsSqlServer::accept_loop() {
        while (running_.load()) {
            const socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd == INVALID_SOCKET_VAL) {
                if (!running_.load())
                    return;
                continue;
            }
            // 线程/连接：阻塞模型，断开时线程结束。
            std::thread([this, fd] {
                try {
                    serve_client(fd);
                } catch (...) {
                    // TLS 握手失败或 IO 错误：直接关闭。
                }
                close_socket(fd);
            }).detach();
        }
    }

    void TlsSqlServer::serve_client(socket_t fd) {
        auto stream = tls::TlsStream::accept(tls_, fd);

        auto session = std::make_shared<Session>();
        session->statement_timeout_ms = Config::instance().statement_timeout_ms();
        const std::size_t max_line = Config::instance().max_buffer_size();

        std::string linebuf;
        std::string plaintext;
        char buf[16384];
        for (;;) {
            // 取一行（\n 结尾；超长断连）。
            auto nl = linebuf.find('\n');
            while (nl == std::string::npos) {
                const std::size_t n = stream->read(buf, sizeof(buf));
                if (n == 0)
                    return; // 对端关闭
                linebuf.append(buf, n);
                if (linebuf.size() > max_line)
                    return; // 防内存耗尽
                nl = linebuf.find('\n');
            }
            std::string line = linebuf.substr(0, nl);
            linebuf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.find_first_not_of(" \t") == std::string::npos)
                continue;

            const std::string response =
                    sql_proto::process_sql_line(db_, std::move(line), session);
            if (!response.empty())
                stream->write(response);
        }
    }

} // namespace corodb
