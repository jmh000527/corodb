/**
 * @file test_security.cpp
 * @brief 安全测试（ROADMAP P2）：RBAC 角色权限 + 审计日志。
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "corodb/common/audit.h"
#include "corodb/common/config.h"
#include "corodb/common/crypto.h"
#include "corodb/db/database.h"
#include "corodb/db/session.h"
#include "corodb/storage/storage_engine_common.h"

using namespace corodb;

namespace {
    class TempDir {
    public:
        explicit TempDir(const std::string& prefix) {
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(now));
            std::filesystem::create_directories(path_);
        }
        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
        std::string path() const {
            return path_.string();
        }

    private:
        std::filesystem::path path_;
    };
} // namespace

// ============================================================================
// RBAC：角色权限
// ============================================================================

TEST(RbacTest, ReadOnlyRoleCanQueryButNotModify) {
    TempDir dir("corodb_rbac");
    Database db(dir.path());

    // admin 建表 + 建一个只读用户。
    ASSERT_TRUE(db.execute("CREATE TABLE r (id INT)").is_success());
    ASSERT_TRUE(db.execute("INSERT INTO r VALUES (1)").is_success());
    ASSERT_TRUE(db.execute("CREATE USER viewer 'pw' ROLE read_only").is_success());

    auto sess = std::make_shared<Session>();
    auto auth = db.execute("AUTH viewer 'pw'", sess);
    ASSERT_TRUE(auth.is_success());
    EXPECT_EQ(sess->auth_role, UserRole::ReadOnly);

    // 查询允许。
    auto sel = db.execute("SELECT * FROM r", sess);
    EXPECT_TRUE(sel.is_success() || sel.rows.has_value());
    // 一切变更被拒。
    EXPECT_THROW(db.execute("INSERT INTO r VALUES (2)", sess), std::runtime_error);
    EXPECT_THROW(db.execute("UPDATE r SET id = 3", sess), std::runtime_error);
    EXPECT_THROW(db.execute("DELETE FROM r", sess), std::runtime_error);
    EXPECT_THROW(db.execute("CREATE TABLE x (a INT)", sess), std::runtime_error);
    EXPECT_THROW(db.execute("DROP TABLE r", sess), std::runtime_error);
    EXPECT_THROW(db.execute("EXPLAIN ANALYZE INSERT INTO r VALUES (9)", sess), std::runtime_error);
}

TEST(RbacTest, ReadWriteRoleCanModifyButNotCreateUsers) {
    TempDir dir("corodb_rbac_rw");
    Database db(dir.path());
    ASSERT_TRUE(db.execute("CREATE USER writer 'pw' ROLE read_write").is_success());

    auto sess = std::make_shared<Session>();
    ASSERT_TRUE(db.execute("AUTH writer 'pw'", sess).is_success());
    EXPECT_EQ(sess->auth_role, UserRole::ReadWrite);

    ASSERT_TRUE(db.execute("CREATE TABLE w (id INT)", sess).is_success());
    ASSERT_TRUE(db.execute("INSERT INTO w VALUES (1)", sess).is_success());
    ASSERT_TRUE(db.execute("UPDATE w SET id = 2", sess).is_success());
    ASSERT_TRUE(db.execute("DELETE FROM w WHERE id = 2", sess).is_success());
    // 用户管理是 admin 专属。
    EXPECT_THROW(db.execute("CREATE USER hacker 'x'", sess), std::runtime_error);
}

TEST(RbacTest, AdminRoleCanCreateUsers) {
    TempDir dir("corodb_rbac_admin");
    Database db(dir.path());

    // 引导例外：无任何用户时允许匿名创建首个用户（admin）。
    ASSERT_TRUE(db.execute("CREATE USER boss 'pw' ROLE admin").is_success());
    // 此后匿名 CREATE USER 必须被拒绝（既有漏洞修复：不得绕过认证开户）。
    EXPECT_THROW(db.execute("CREATE USER attacker 'pw'"), std::runtime_error);

    auto sess = std::make_shared<Session>();
    ASSERT_TRUE(db.execute("AUTH boss 'pw'", sess).is_success());
    EXPECT_EQ(sess->auth_role, UserRole::Admin);
    ASSERT_TRUE(db.execute("CREATE USER worker 'pw' ROLE read_only", sess).is_success());
}

// ============================================================================
// 审计日志
// ============================================================================

TEST(AuditTest, StatementsAreLoggedAsJsonLines) {
    TempDir dir("corodb_audit");
    const std::string audit_path = (std::filesystem::path(dir.path()) / "audit.log").string();
    Config::instance().set_audit_path(audit_path);
    Config::instance().set_audit_enabled(true);

    Database db(dir.path());
    ASSERT_TRUE(db.execute("CREATE TABLE a (id INT)").is_success());
    EXPECT_THROW(db.execute("INSERT INTO missing VALUES (1)"), std::runtime_error);

    // 读取审计文件（重建 Database 前直接读）。
    ASSERT_TRUE(std::filesystem::exists(audit_path));
    std::ifstream ifs(audit_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"ts\":\""), std::string::npos);
    EXPECT_NE(content.find("\"role\":\"anonymous\""), std::string::npos);
    EXPECT_NE(content.find("\"sql\":\"CREATE TABLE a (id INT)\""), std::string::npos);
    EXPECT_NE(content.find("\"status\":\"ok\""), std::string::npos);
    // 失败语句：status=error + 错误消息。
    EXPECT_NE(content.find("\"status\":\"error\""), std::string::npos);
    EXPECT_NE(content.find("\"duration_ms\":"), std::string::npos);
}

TEST(AuditTest, DisabledAuditWritesNothing) {
    TempDir dir("corodb_audit_off");
    const std::string audit_path = (std::filesystem::path(dir.path()) / "audit.log").string();
    Config::instance().set_audit_path(audit_path);
    Config::instance().set_audit_enabled(false);

    Database db(dir.path());
    ASSERT_TRUE(db.execute("CREATE TABLE zz (a INT)").is_success());
    EXPECT_FALSE(std::filesystem::exists(audit_path));
}

// ============================================================================
// 限流（P4）：令牌桶
// ============================================================================

TEST(RateLimitTest, ConfigRoundTrip) {
    auto& c = Config::instance();
    const uint32_t saved = c.rate_limit_per_sec();
    c.set_rate_limit_per_sec(5);
    EXPECT_EQ(c.rate_limit_per_sec(), 5u);
    c.set_rate_limit_per_sec(saved);
}

TEST(RateLimitTest, DefaultDisabled) {
    // 默认 0 = 禁用（不改变全局配置即可断言默认值语义）。
    EXPECT_EQ(Config::kDefaultRateLimitPerSec, 0u);
}
