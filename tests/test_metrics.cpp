/**
 * @file test_metrics.cpp
 * @brief 观测性测试（ROADMAP P2）：指标注册表 + Prometheus 渲染 + 管理端 HTTP 端点。
 */

#include <gtest/gtest.h>

#include <string>

#include "corodb/common/metrics.h"
#include "corodb/net/port.h"
#include "corodb/server/admin_server.h"

using namespace corodb;

// ============================================================================
// 指标注册表
// ============================================================================

TEST(MetricsTest, CounterGaugeRoundTrip) {
    auto& m = Metrics::instance();
    auto& c = m.counter("test_counter_a", {}, "help text");
    auto& g = m.gauge("test_gauge_a", {}, "gauge help");
    const uint64_t before = c.value();
    c.increment();
    c.increment(5);
    EXPECT_EQ(c.value(), before + 6);
    g.set(42);
    EXPECT_EQ(g.value(), 42);
    g.increment();
    g.decrement(2);
    EXPECT_EQ(g.value(), 41);
}

TEST(MetricsTest, LabeledCounterIsDistinct) {
    auto& m = Metrics::instance();
    auto& ok = m.counter("test_labeled", R"({result="ok"})");
    auto& err = m.counter("test_labeled", R"({result="error"})");
    const uint64_t ok0 = ok.value();
    const uint64_t err0 = err.value();
    ok.increment();
    EXPECT_EQ(ok.value(), ok0 + 1);
    EXPECT_EQ(err.value(), err0); // 标签不同 → 独立序列
}

TEST(MetricsTest, PrometheusRenderFormat) {
    auto& m = Metrics::instance();
    auto& c = m.counter("test_render_counter", R"({result="ok"})", "Test render help");
    c.increment(7);
    auto& g = m.gauge("test_render_gauge", {}, "Test gauge help");
    g.set(3);
    auto& h = m.histogram("test_render_histogram_seconds", {0.1, 1.0}, "Test histogram help");
    h.observe(0.05);
    h.observe(0.5);
    h.observe(2.0);

    const std::string out = m.render_prometheus();
    // counter：名字 + 标签 + 值；TYPE/HELP 行存在。
    EXPECT_NE(out.find("# HELP test_render_counter Test render help"), std::string::npos) << out;
    EXPECT_NE(out.find("# TYPE test_render_counter counter"), std::string::npos) << out;
    EXPECT_NE(out.find("test_render_counter{result=\"ok\"} 7"), std::string::npos) << out;
    // gauge。
    EXPECT_NE(out.find("# TYPE test_render_gauge gauge"), std::string::npos) << out;
    EXPECT_NE(out.find("test_render_gauge 3"), std::string::npos) << out;
    // histogram：累积桶（0.1 桶 = 1，1.0 桶 = 2，+Inf = 3）+ sum/count。
    EXPECT_NE(out.find("test_render_histogram_seconds_bucket{le=\"0.1\"} 1"), std::string::npos) << out;
    EXPECT_NE(out.find("test_render_histogram_seconds_bucket{le=\"1\"} 2"), std::string::npos) << out;
    EXPECT_NE(out.find("test_render_histogram_seconds_bucket{le=\"+Inf\"} 3"), std::string::npos) << out;
    EXPECT_NE(out.find("test_render_histogram_seconds_count 3"), std::string::npos) << out;
    // 输出以换行结尾（Prometheus 文本格式约定）。
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.back(), '\n');
}

// ============================================================================
// 管理端 HTTP 端点
// ============================================================================

namespace {
    std::string http_get(uint16_t port, const std::string& target) {
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET_VAL)
            return {};
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return {};
        }
        const std::string req = "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        if (write_socket(fd, req.data(), req.size()) < 0) {
            close_socket(fd);
            return {};
        }
        std::string resp;
        char buf[2048];
        for (;;) {
            const int n = read_socket(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            resp.append(buf, static_cast<std::size_t>(n));
        }
        close_socket(fd);
        return resp;
    }
} // namespace

TEST(AdminServerTest, ServesMetricsAndHealthz) {
    AdminServer::Options opts;
    opts.port = 0; // OS 分配端口，避免测试冲突
    opts.handler = [](const std::string& path) {
        if (path == "/healthz")
            return std::string("ok\n");
        if (path == "/metrics") {
            // 确保 /metrics 序列至少有一条指标。
            Metrics::instance().counter("test_admin_counter", {}).increment();
            return Metrics::instance().render_prometheus();
        }
        return std::string();
    };
    AdminServer server(std::move(opts));
    ASSERT_NO_THROW(server.start());
    ASSERT_GT(server.port(), 0);

    const std::string health = http_get(server.port(), "/healthz");
    EXPECT_NE(health.find("HTTP/1.1 200 OK"), std::string::npos) << health;
    EXPECT_NE(health.find("ok"), std::string::npos) << health;

    const std::string metrics = http_get(server.port(), "/metrics");
    EXPECT_NE(metrics.find("HTTP/1.1 200 OK"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("corodb_"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("test_admin_counter"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("Content-Length:"), std::string::npos) << metrics;

    const std::string missing = http_get(server.port(), "/nope");
    EXPECT_NE(missing.find("404"), std::string::npos) << missing;
}

TEST(AdminServerTest, UnknownPathReturns404WithoutHandler) {
    AdminServer::Options opts;
    opts.port = 0;
    AdminServer server(std::move(opts)); // 无 handler
    ASSERT_NO_THROW(server.start());
    const std::string resp = http_get(server.port(), "/anything");
    EXPECT_NE(resp.find("404"), std::string::npos) << resp;
}
