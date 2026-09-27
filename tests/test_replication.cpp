/**
 * @file test_replication.cpp
 * @brief WAL 日志复制测试（ROADMAP P4）：记录编解码 + 端到端流同步 + 只读副本。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "corodb/common/types.h"
#include "corodb/db/database.h"
#include "corodb/replication/replication.h"
#include "corodb/storage/lsm_storage_engine.h"
#include "corodb/storage/storage_engine_common.h"

using namespace corodb;

namespace {
    class TempDirPair {
    public:
        TempDirPair() {
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            a_ = std::filesystem::temp_directory_path() / ("repl_primary_" + std::to_string(now));
            b_ = std::filesystem::temp_directory_path() / ("repl_replica_" + std::to_string(now));
            std::filesystem::create_directories(a_);
            std::filesystem::create_directories(b_);
        }
        ~TempDirPair() {
            std::error_code ec;
            std::filesystem::remove_all(a_, ec);
            std::filesystem::remove_all(b_, ec);
        }
        const std::filesystem::path& primary() const {
            return a_;
        }
        const std::filesystem::path& replica() const {
            return b_;
        }

    private:
        std::filesystem::path a_, b_;
    };

    /** @brief 轮询直到条件成立或超时（复制是异步的，需等待收敛）。 */
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
} // namespace

// ============================================================================
// 记录编解码
// ============================================================================

TEST(ReplicationRecord, RoundTripAllTypes) {
    const std::vector<Column> cols = { Column{ "t", "id", TypeKind::Int64 },
                                       Column{ "t", "name", TypeKind::Text } };

    ReplicationRecord row;
    row.type = ReplicationRecord::Type::ApplyRow;
    row.table = "t";
    row.commit_ts = 42;
    row.row_wire = storage_internal::encode_row(Row{ std::vector<Value>{ int64_t{ 7 }, std::string{ "x" } } });
    auto d1 = ReplicationRecord();
    ASSERT_TRUE(d1.deserialize(row.serialize()));
    EXPECT_EQ(d1.type, ReplicationRecord::Type::ApplyRow);
    EXPECT_EQ(d1.table, "t");
    EXPECT_EQ(d1.commit_ts, 42u);
    EXPECT_EQ(d1.row_wire, row.row_wire);

    ReplicationRecord del;
    del.type = ReplicationRecord::Type::ApplyDelete;
    del.table = "t";
    del.commit_ts = 43;
    del.row_wire = storage_internal::encode_key(Value{ int64_t{ 7 } });
    ReplicationRecord d2;
    ASSERT_TRUE(d2.deserialize(del.serialize()));
    EXPECT_EQ(d2.type, ReplicationRecord::Type::ApplyDelete);
    EXPECT_EQ(d2.row_wire, del.row_wire);

    ReplicationRecord commit;
    commit.type = ReplicationRecord::Type::Commit;
    commit.commit_ts = 44;
    ReplicationRecord d3;
    ASSERT_TRUE(d3.deserialize(commit.serialize()));
    EXPECT_EQ(d3.commit_ts, 44u);

    ReplicationRecord create;
    create.type = ReplicationRecord::Type::CreateTable;
    create.table = "t";
    create.columns = cols;
    ReplicationRecord d4;
    ASSERT_TRUE(d4.deserialize(create.serialize()));
    EXPECT_EQ(d4.columns.size(), 2u);
    EXPECT_EQ(d4.columns[1].name, "name");

    ReplicationRecord drop;
    drop.type = ReplicationRecord::Type::DropTable;
    drop.table = "t";
    ReplicationRecord d5;
    ASSERT_TRUE(d5.deserialize(drop.serialize()));
    EXPECT_EQ(d5.table, "t");

    // 截断的 payload 必须失败而不是崩溃。
    ReplicationRecord bad;
    EXPECT_FALSE(bad.deserialize("g"));
}

// ============================================================================
// 端到端流同步 + 只读副本
// ============================================================================

TEST(ReplicationEndToEnd, PrimaryStreamsToReadOnlyReplica) {
    TempDirPair dirs;
    Database primary(dirs.primary().string());
    Database replica(dirs.replica().string());

    auto* primary_lsm = static_cast<LSMTreeEngine*>(primary.get_storage());
    auto* replica_lsm = static_cast<LSMTreeEngine*>(replica.get_storage());

    // 主端：引擎变更 → hub 广播；从端：接收 → 应用 → DDL 后刷新 Catalog。
    ReplicationHub hub;
    hub.start(0); // OS 分配端口
    primary_lsm->set_replication_sink([&hub](const ReplicationRecord& rec) { hub.broadcast(rec); });

    ReplicationFollower follower;
    follower.start("127.0.0.1", hub.port(), [&replica, replica_lsm](const ReplicationRecord& rec) {
        if (replica_lsm->apply_replication_record(rec) &&
            (rec.type == ReplicationRecord::Type::CreateTable ||
             rec.type == ReplicationRecord::Type::DropTable)) {
            replica.reload_catalog();
        }
    });
    ASSERT_TRUE(poll_until([&] { return follower.connected(); }));

    // 只读副本：写语句在查询层被拒绝（与 server 装配 replica 角色一致）。
    replica.set_read_only(true);

    // 主端建表 + 写入（auto-commit 路径）。
    ASSERT_TRUE(primary.execute("CREATE TABLE rt (id INT, val TEXT)").is_success());
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(primary
                        .execute("INSERT INTO rt VALUES (" + std::to_string(i) + ", 'v" + std::to_string(i) + "')")
                        .is_success());
    }

    // 从端异步收敛：行数最终一致（表注册前查询会抛异常，轮询闭包容错）。
    auto count_on = [](Database& db, const std::string& sql) -> std::string {
        try {
            auto r = db.execute(sql);
            std::string out;
            if (r.rows.has_value()) {
                for (auto&& rec : *r.rows)
                    for (const auto& v : rec.values)
                        if (std::holds_alternative<int64_t>(v))
                            out = std::to_string(std::get<int64_t>(v));
            }
            return out;
        } catch (...) {
            return "";
        }
    };
    EXPECT_TRUE(poll_until([&] { return count_on(replica, "SELECT COUNT(*) FROM rt") == "20"; }));

    // 数据内容一致（含主键点查路径）。
    auto r = replica.execute("SELECT val FROM rt WHERE id = 5");
    ASSERT_TRUE(r.rows.has_value());
    std::string val;
    for (auto&& rec : *r.rows)
        for (const auto& v : rec.values)
            if (std::holds_alternative<std::string>(v))
                val = std::get<std::string>(v);
    EXPECT_EQ(val, "v5");

    // 从端拒绝写入（只读强制）。
    EXPECT_THROW(replica.execute("INSERT INTO rt VALUES (99, 'no')"), std::runtime_error);
    EXPECT_THROW(replica.execute("DROP TABLE rt"), std::runtime_error);
    EXPECT_THROW(replica.execute("ANALYZE TABLE rt"), std::runtime_error);
    // 只读查询不受影响。
    EXPECT_TRUE(replica.execute("SELECT * FROM rt").is_success());

    // 主端 DELETE 同步到从端。
    ASSERT_TRUE(primary.execute("DELETE FROM rt WHERE id = 5").is_success());
    EXPECT_EQ(poll_until([&] { return count_on(replica, "SELECT COUNT(*) FROM rt") == "19"; }), true);

    follower.stop();
    hub.stop();
}

TEST(ReplicationEndToEnd, FollowerUnknownTableSkipsGracefully) {
    // 未知表的行记录：apply 返回 false，接收循环不崩溃（单进程直调验证）。
    TempDirPair dirs;
    Database replica(dirs.replica().string());
    auto* replica_lsm = static_cast<LSMTreeEngine*>(replica.get_storage());

    ReplicationRecord rec;
    rec.type = ReplicationRecord::Type::ApplyRow;
    rec.table = "ghost";
    rec.commit_ts = 1;
    rec.row_wire = "junk";
    EXPECT_FALSE(replica_lsm->apply_replication_record(rec));
}
