// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file admin_server.cpp
// @brief 管理端 HTTP 服务实现（/metrics、/healthz）。

#include "corodb/server/admin_server.h"

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace corodb {

    namespace {

        void write_all(socket_t fd, const std::string& data) {
            std::size_t sent = 0;
            while (sent < data.size()) {
                const int n = write_socket(fd, data.data() + sent, data.size() - sent);
                if (n <= 0)
                    return;
                sent += static_cast<std::size_t>(n);
            }
        }

        void respond(socket_t fd, int status, const std::string& body) {
            const char* reason = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Bad Request");
            std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                               "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            write_all(fd, head);
            write_all(fd, body);
        }

    } // namespace

    AdminServer::AdminServer(Options opt) : opts_(std::move(opt)) {
    }

    AdminServer::~AdminServer() {
        stop();
    }

    void AdminServer::start() {
#ifdef _WIN32
        // WSAStartup 由服务器入口统一调用；此处兜底（测试直连场景）。
        static std::once_flag wsa_flag;
        std::call_once(wsa_flag, [] {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET_VAL)
            throw std::runtime_error("[AdminServer] socket() failed");

        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本机回环：指标端点不暴露给外网
        addr.sin_port = htons(opts_.port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[AdminServer] bind() failed on port " + std::to_string(opts_.port));
        }
        if (::listen(listen_fd_, 16) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[AdminServer] listen() failed");
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

    void AdminServer::stop() {
        if (!running_.exchange(false))
            return;
        if (listen_fd_ != INVALID_SOCKET_VAL) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
        }
        if (accept_thread_.joinable())
            accept_thread_.join();
    }

    void AdminServer::accept_loop() {
        while (running_.load()) {
            const socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd == INVALID_SOCKET_VAL) {
                if (!running_.load())
                    return;
                continue;
            }
            handle_client(fd);
            close_socket(fd);
        }
    }

    void AdminServer::handle_client(socket_t fd) {
        // 读取请求头（直到 \r\n\r\n 或上限）；scrape 请求很小，8KB 足够。
        std::string req;
        char buf[1024];
        while (req.size() < 8192) {
            const int n = read_socket(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            req.append(buf, static_cast<std::size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos)
                break;
        }
        // 请求行：METHOD SP PATH SP VERSION
        const auto line_end = req.find("\r\n");
        if (line_end == std::string::npos) {
            respond(fd, 400, "bad request");
            return;
        }
        const std::string line = req.substr(0, line_end);
        const auto sp1 = line.find(' ');
        const auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            respond(fd, 400, "bad request");
            return;
        }
        const std::string method = line.substr(0, sp1);
        std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const auto query = path.find('?');
        if (query != std::string::npos)
            path.resize(query);

        if (method != "GET" && method != "HEAD") {
            respond(fd, 404, "method not allowed");
            return;
        }
        std::string body;
        if (opts_.handler) {
            body = opts_.handler(path);
        }
        if (body.empty() && path != "/metrics" && path != "/healthz") {
            respond(fd, 404, "not found");
            return;
        }
        respond(fd, 200, body);
    }

} // namespace corodb
