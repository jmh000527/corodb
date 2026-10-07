/**
 * @file test_tls.cpp
 * @brief TLS 传输加密测试（ROADMAP P2）：HTTPS / 复制通道 / SQL TLS 端口。
 *
 * Windows 走 Schannel（运行时自签名证书）；Linux 走 OpenSSL。客户端使用
 * verify_cert=false 接受自签名服务器。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "corodb/common/config.h"
#include "corodb/db/database.h"
#include "corodb/net/port.h"
#include "corodb/net/tls.h"
#include "corodb/replication/replication.h"
#include "corodb/server/admin_server.h"
#include "corodb/server/tls_sql_server.h"
#include "corodb/storage/lsm_storage_engine.h"

#ifndef CORODB_TEST_CERT_DIR
#define CORODB_TEST_CERT_DIR "tests/test_certs"
#endif

using namespace corodb;

namespace {
    /** @brief 轮询直到条件成立或超时。 */
    template <typename Fn>
    bool poll_until(Fn&& fn, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (fn())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return fn();
    }

    struct TlsFixture {
        std::shared_ptr<tls::TlsContext> server;
        std::shared_ptr<tls::TlsContext> client;
        static const TlsFixture& get() {
            static TlsFixture fx; // 一次性构建，全部 TLS 测试共用
            return fx;
        }
        TlsFixture() {
            // 测试证书：tests/test_certs/{server.crt,server.key}（自签名 CN=corodb，已入库）。
            std::filesystem::path base = CORODB_TEST_CERT_DIR;
            tls::TlsConfig cfg;
            cfg.cert_path = (base / "server.crt").string();
            cfg.key_path = (base / "server.key").string();
            cfg.verify_cert = false; // 自签名：客户端手动信任
            server = tls::TlsContext::create_server(cfg);
            client = tls::TlsContext::create_client(cfg);
        }
    };

    /** @brief 阻塞读一行（含 \n）。 */
    std::string stream_read_line(tls::TlsStream& s) {
        std::string line;
        char c = 0;
        while (s.read(&c, 1) == 1) {
            line.push_back(c);
            if (c == '\n')
                break;
        }
        return line;
    }
} // namespace

TEST(TlsAdmin, HttpsHealthzAndMetrics) {
    const TlsFixture& fx = TlsFixture::get();
    ASSERT_TRUE(fx.server != nullptr);

    AdminServer::Options opts;
    opts.port = 0;
    opts.tls = fx.server;
    opts.handler = [](const std::string& path) {
        if (path == "/healthz")
            return std::string("ok\n");
        return std::string();
    };
    AdminServer server(std::move(opts));
    std::fprintf(stderr, "[tls fx] admin constructed\n");
    ASSERT_NO_THROW(server.start());
    std::fprintf(stderr, "[tls fx] admin started port=%d\n", (int)server.port());

    // TLS 客户端连接 → HTTPS GET。
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(fd, INVALID_SOCKET_VAL);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(server.port());
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    std::fprintf(stderr, "[tls fx] tcp connected client=%p use=%d\n", (void*)fx.client.get(),
                 (int)fx.client.use_count());
    auto stream = tls::TlsStream::connect(fx.client, fd, "localhost");
    std::fprintf(stderr, "[tls fx] tls connected\n");
    stream->write("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string resp;
    char buf[4096];
    for (int i = 0; i < 20; ++i) {
        const std::size_t n = stream->read(buf, sizeof(buf));
        if (n == 0)
            break;
        resp.append(buf, n);
        if (resp.find("\r\n\r\n") != std::string::npos && resp.find("ok") != std::string::npos)
            break;
    }
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("ok"), std::string::npos) << resp;
    close_socket(fd);
}

TEST(TlsReplication, EndToEndOverTls) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const auto dir_a = std::filesystem::temp_directory_path() / ("tls_repl_p_" + std::to_string(now));
    const auto dir_b = std::filesystem::temp_directory_path() / ("tls_repl_r_" + std::to_string(now));
    std::filesystem::create_directories(dir_a);
    std::filesystem::create_directories(dir_b);

    Database primary(dir_a.string());
    Database replica(dir_b.string());
    auto* plsm = static_cast<LSMTreeEngine*>(primary.get_storage());
    auto* rlsm = static_cast<LSMTreeEngine*>(replica.get_storage());

    const TlsFixture& fx = TlsFixture::get();
    ReplicationHub hub;
    hub.set_tls(fx.server);
    hub.start(0);
    plsm->set_replication_sink([&hub](const ReplicationRecord& rec) { hub.broadcast(rec); });

    ReplicationFollower follower;
    follower.set_tls(fx.client);
    follower.start("127.0.0.1", hub.port(), [&replica, rlsm](const ReplicationRecord& rec) {
        if (rlsm->apply_replication_record(rec) && rec.type == ReplicationRecord::Type::CreateTable)
            replica.reload_catalog();
    });

    ASSERT_TRUE(primary.execute("CREATE TABLE tt (id INT)").is_success());
    for (int i = 0; i < 10; ++i)
        ASSERT_TRUE(primary.execute("INSERT INTO tt VALUES (" + std::to_string(i) + ")").is_success());

    auto count_on = [](Database& db) -> std::string {
        try {
            auto r = db.execute("SELECT COUNT(*) FROM tt");
            std::string out;
            if (r.rows.has_value())
                for (auto&& rec : *r.rows)
                    for (const auto& v : rec.values)
                        if (std::holds_alternative<int64_t>(v))
                            out = std::to_string(std::get<int64_t>(v));
            return out;
        } catch (...) {
            return "";
        }
    };
    EXPECT_TRUE(poll_until([&] { return count_on(replica) == "10"; }));

    follower.stop();
    hub.stop();
    std::error_code ec;
    std::filesystem::remove_all(dir_a, ec);
    std::filesystem::remove_all(dir_b, ec);
}

TEST(TlsSqlServerTest, ServeSqlOverTls) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() / ("tls_sql_" + std::to_string(now));
    std::filesystem::create_directories(dir);
    Database db(dir.string());
    ASSERT_TRUE(db.execute("CREATE TABLE s (id INT, val TEXT)").is_success());
    ASSERT_TRUE(db.execute("INSERT INTO s VALUES (1, 'secret-over-tls')").is_success());

    const TlsFixture& fx = TlsFixture::get();
    TlsSqlServer listener(0, fx.server, db);
    ASSERT_NO_THROW(listener.start());

    // 客户端：TLS 连接 + 文本协议。
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(fd, INVALID_SOCKET_VAL);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(listener.port());
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    std::fprintf(stderr, "[tls fx] tcp connected, tls connecting\n");
    auto stream = tls::TlsStream::connect(fx.client, fd, "localhost");
    std::fprintf(stderr, "[tls fx] tls connected\n");
    stream->write("SELECT val FROM s WHERE id = 1;\n");

    std::string resp;
    char buf[4096];
    for (int i = 0; i < 30; ++i) {
        const std::size_t n = stream->read(buf, sizeof(buf));
        if (n == 0)
            break;
        resp.append(buf, n);
        if (resp.find("@END") != std::string::npos)
            break;
    }
    EXPECT_NE(resp.find("@TABLE"), std::string::npos) << resp;
    EXPECT_NE(resp.find("secret-over-tls"), std::string::npos) << resp;
    EXPECT_NE(resp.find("@END"), std::string::npos) << resp;
    close_socket(fd);

    listener.stop();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// 诊断：双侧握手异常直接暴露
