// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file replication.cpp
// @brief WAL 日志复制实现：记录编解码 + 主端推送 + 从端应用。

#include "corodb/replication/replication.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "corodb/storage/storage_engine_common.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace corodb {

    namespace {
        void send_all(socket_t fd, const char* data, std::size_t n) {
            std::size_t sent = 0;
            while (sent < n) {
                const int k = write_socket(fd, data + sent, static_cast<int>(n - sent));
                if (k <= 0)
                    throw std::runtime_error("[Replication] socket send failed");
                sent += static_cast<std::size_t>(k);
            }
        }
    } // namespace

    // =========================================================================
    // ReplicationRecord
    // =========================================================================

    std::string ReplicationRecord::serialize() const {
        std::string out;
        out.push_back(static_cast<char>(type));
        switch (type) {
            case Type::ApplyRow: {
                out.push_back(static_cast<char>(table.size()));
                out.append(table);
                out.append(reinterpret_cast<const char*>(&commit_ts), sizeof(commit_ts));
                uint32_t len = static_cast<uint32_t>(row_wire.size());
                out.append(reinterpret_cast<const char*>(&len), sizeof(len));
                out.append(row_wire);
                break;
            }
            case Type::ApplyDelete: {
                out.push_back(static_cast<char>(table.size()));
                out.append(table);
                out.append(reinterpret_cast<const char*>(&commit_ts), sizeof(commit_ts));
                uint32_t len = static_cast<uint32_t>(row_wire.size());
                out.append(reinterpret_cast<const char*>(&len), sizeof(len));
                out.append(row_wire);
                break;
            }
            case Type::Commit: {
                out.append(reinterpret_cast<const char*>(&commit_ts), sizeof(commit_ts));
                break;
            }
            case Type::CreateTable: {
                out.push_back(static_cast<char>(table.size()));
                out.append(table);
                const auto schema = storage_internal::serialize_schema(columns);
                uint32_t len = static_cast<uint32_t>(schema.size());
                out.append(reinterpret_cast<const char*>(&len), sizeof(len));
                out.append(schema.begin(), schema.end());
                break;
            }
            case Type::DropTable: {
                out.push_back(static_cast<char>(table.size()));
                out.append(table);
                break;
            }
            default:
                throw std::runtime_error("[Replication] unknown record type");
        }
        return out;
    }

    bool ReplicationRecord::deserialize(const std::string& payload) {
        if (payload.empty())
            return false;
        const char* p = payload.data();
        std::size_t left = payload.size();
        type = static_cast<Type>(static_cast<unsigned char>(p[0]));
        ++p;
        --left;
        auto read_bytes = [&](std::size_t n) -> const char* {
            if (left < n)
                return nullptr;
            const char* out = p;
            p += n;
            left -= n;
            return out;
        };
        auto read_u8 = [&](unsigned char& v) {
            const char* b = read_bytes(1);
            if (!b)
                return false;
            v = static_cast<unsigned char>(b[0]);
            return true;
        };
        auto read_u32 = [&](uint32_t& v) {
            const char* b = read_bytes(4);
            if (!b)
                return false;
            std::memcpy(&v, b, 4);
            return true;
        };
        auto read_u64 = [&](uint64_t& v) {
            const char* b = read_bytes(8);
            if (!b)
                return false;
            std::memcpy(&v, b, 8);
            return true;
        };
        auto read_table = [&](std::string& name) {
            unsigned char len = 0;
            if (!read_u8(len))
                return false;
            const char* b = read_bytes(len);
            if (!b)
                return false;
            name.assign(b, len);
            return true;
        };

        switch (type) {
            case Type::ApplyRow: {
                if (!read_table(table) || !read_u64(commit_ts))
                    return false;
                uint32_t len = 0;
                if (!read_u32(len))
                    return false;
                const char* b = read_bytes(len);
                if (!b)
                    return false;
                row_wire.assign(b, len);
                return true;
            }
            case Type::ApplyDelete: {
                if (!read_table(table) || !read_u64(commit_ts))
                    return false;
                uint32_t len = 0;
                if (!read_u32(len))
                    return false;
                const char* b = read_bytes(len);
                if (!b)
                    return false;
                row_wire.assign(b, len);
                return true;
            }
            case Type::Commit:
                return read_u64(commit_ts);
            case Type::CreateTable: {
                if (!read_table(table))
                    return false;
                uint32_t len = 0;
                if (!read_u32(len))
                    return false;
                const char* b = read_bytes(len);
                if (!b)
                    return false;
                std::string schema_str(b, len);
                std::istringstream iss(schema_str);
                columns = storage_internal::deserialize_schema(iss, table);
                return !columns.empty();
            }
            case Type::DropTable:
                return read_table(table);
            default:
                return false;
        }
    }

    std::string ReplicationRecord::frame() const {
        const std::string payload = serialize();
        uint32_t len = static_cast<uint32_t>(payload.size());
        std::string out(reinterpret_cast<const char*>(&len), sizeof(len));
        out += payload;
        return out;
    }

    // =========================================================================
    // ReplicationHub（主端）
    // =========================================================================

    ReplicationHub::ReplicationHub()
        : sent_counter_(Metrics::instance().counter(
                  "corodb_replication_records_sent_total", {},
                  "Replication records sent to followers since startup.")),
          connected_gauge_(Metrics::instance().gauge(
                  "corodb_replication_followers_connected", {},
                  "Currently connected followers.")) {
    }

    ReplicationHub::~ReplicationHub() {
        stop();
    }

    void ReplicationHub::start(uint16_t port) {
#ifdef _WIN32
        static std::once_flag wsa_flag;
        std::call_once(wsa_flag, [] {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET_VAL)
            throw std::runtime_error("[ReplicationHub] socket() failed");
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[ReplicationHub] bind() failed on port " + std::to_string(port));
        }
        if (::listen(listen_fd_, 16) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
            throw std::runtime_error("[ReplicationHub] listen() failed");
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
            port_.store(ntohs(bound.sin_port));
        running_.store(true);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    void ReplicationHub::stop() {
        if (!running_.exchange(false))
            return;
        if (listen_fd_ != INVALID_SOCKET_VAL) {
            close_socket(listen_fd_);
            listen_fd_ = INVALID_SOCKET_VAL;
        }
        if (accept_thread_.joinable())
            accept_thread_.join();
        std::vector<std::shared_ptr<Client>> clients;
        {
            std::lock_guard lk(clients_mutex_);
            clients.swap(clients_);
        }
        for (auto& c: clients)
            c->alive.store(false);
        // 关闭 fd 解开 sender 的阻塞 send。
        for (auto& c: clients)
            close_socket(c->fd);
        // sender 线程为 detach 模式；给一点时间退出，避免析构竞态。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void ReplicationHub::accept_loop() {
        while (running_.load()) {
            const socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd == INVALID_SOCKET_VAL) {
                if (!running_.load())
                    return;
                continue;
            }
            auto client = std::make_shared<Client>();
            client->fd = fd;
            if (tls_) {
                try {
                    client->stream = tls::TlsStream::accept(tls_, fd);
                } catch (...) {
                    close_socket(fd); // 握手失败：拒绝该连接
                    continue;
                }
            }
            {
                std::lock_guard lk(clients_mutex_);
                clients_.push_back(client);
            }
            connected_gauge_.increment();
            std::thread(&ReplicationHub::sender_loop, this, client).detach();
        }
    }

    void ReplicationHub::sender_loop(std::shared_ptr<Client> client) {
        while (client->alive.load()) {
            std::string frame;
            {
                std::lock_guard lk(client->mutex);
                if (client->outbox.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                frame = std::move(client->outbox.front());
                client->outbox.pop();
            }
            try {
                if (client->stream)
                    client->stream->write(frame);
                else
                    send_all(client->fd, frame.data(), frame.size());
            } catch (...) {
                client->alive.store(false);
                connected_gauge_.decrement();
                close_socket(client->fd);
                return;
            }
        }
    }

    void ReplicationHub::reap_dead_clients() {
        std::lock_guard lk(clients_mutex_);
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [](const std::shared_ptr<Client>& c) { return !c->alive.load(); }),
                       clients_.end());
    }

    std::size_t ReplicationHub::follower_count() const {
        std::lock_guard lk(clients_mutex_);
        return clients_.size();
    }

    void ReplicationHub::broadcast(const ReplicationRecord& record) {
        if (!running_.load() || clients_.empty())
            return;
        const std::string frame = record.frame();
        {
            std::lock_guard lk(clients_mutex_);
            for (auto& c: clients_) {
                if (!c->alive.load())
                    continue;
                std::lock_guard<std::mutex> c_lk(c->mutex);
                c->outbox.push(frame);
            }
        }
        sent_counter_.increment();
        // 顺手回收已断开的连接（sender 线程自行退出）。
        reap_dead_clients();
    }

    // =========================================================================
    // ReplicationFollower（从端）
    // =========================================================================

    ReplicationFollower::ReplicationFollower()
        : applied_counter_(Metrics::instance().counter(
                  "corodb_replication_records_applied_total", {},
                  "Replication records applied since startup.")),
          applied_commit_ts_(Metrics::instance().gauge(
                  "corodb_replication_applied_commit_ts", {},
                  "Latest applied commit timestamp from the primary.")) {
    }

    ReplicationFollower::~ReplicationFollower() {
        stop();
    }

    void ReplicationFollower::start(const std::string& host, uint16_t port, ApplyFn apply) {
        host_ = host;
        port_ = port;
        apply_ = std::move(apply);
        running_.store(true);
        thread_ = std::thread([this] { receive_loop(); });
    }

    void ReplicationFollower::stop() {
        if (!running_.exchange(false))
            return;
        if (fd_ != INVALID_SOCKET_VAL) {
            close_socket(fd_);
            fd_ = INVALID_SOCKET_VAL;
        }
        if (thread_.joinable())
            thread_.join();
        stream_.reset();
    }

    bool ReplicationFollower::try_connect() {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET_VAL)
            return false;
        // getaddrinfo（gethostbyname 已弃用）。
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (::getaddrinfo(host_.c_str(), nullptr, &hints, &result) != 0 || !result) {
            close_socket(fd);
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        ::freeaddrinfo(result);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return false;
        }
        fd_ = fd;
        if (tls_) {
            try {
                stream_ = tls::TlsStream::connect(tls_, fd_, host_.empty() ? "localhost" : host_);
            } catch (...) {
                stream_.reset();
                close_socket(fd_);
                fd_ = INVALID_SOCKET_VAL;
                return false;
            }
        }
        return true;
    }

    void ReplicationFollower::receive_loop() {
        // 断开当前连接（TLS 流 + fd），回到重连循环。
        auto drop_connection = [&] {
            connected_.store(false);
            stream_.reset();
            close_socket(fd_);
            fd_ = INVALID_SOCKET_VAL;
        };
        while (running_.load()) {
            if (!connected_.load()) {
                if (!try_connect()) {
                    // 主端不可达：退避重试。
                    for (int i = 0; i < 50 && running_.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                connected_.store(true);
            }
            // 按帧读取：TLS 流或原始 socket。
            auto read_exact = [&](char* dst, std::size_t n) {
                std::size_t got = 0;
                while (got < n) {
                    if (stream_) {
                        const std::size_t k = stream_->read(dst + got, n - got);
                        if (k == 0)
                            return false; // 对端关闭
                        got += k;
                    } else {
                        const int k = read_socket(fd_, dst + got, static_cast<int>(n - got));
                        if (k <= 0)
                            return false;
                        got += static_cast<std::size_t>(k);
                    }
                }
                return true;
            };
            try {
                // 读一帧：[u32 len][payload]。
                uint32_t len = 0;
                if (!read_exact(reinterpret_cast<char*>(&len), 4) || len == 0 || len > (64u << 20)) {
                    drop_connection();
                    continue;
                }
                std::string payload(len, '\0');
                if (!read_exact(payload.data(), len)) {
                    drop_connection();
                    continue;
                }
                ReplicationRecord rec;
                if (rec.deserialize(payload)) {
                    apply_(rec);
                    applied_counter_.increment();
                    if (rec.type == ReplicationRecord::Type::Commit)
                        applied_commit_ts_.set(static_cast<int64_t>(rec.commit_ts));
                }
                // 反序列化失败：跳过该帧（协议错误场景；不中断流）。
            } catch (const tls::TlsError&) {
                if (!running_.load())
                    return;
                drop_connection();
            }
        }
    }

} // namespace corodb
