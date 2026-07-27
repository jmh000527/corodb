/**
 * @file test_transaction.cpp
 * @brief 事务生命周期与 SET TRANSACTION 隔离级别测试（Phase 1）
 *
 * 当前阶段只验证状态机正确性，不验证 MVCC 可见性差异（后续 Phase 2/3）。
 *
 * 覆盖：
 *   - BEGIN / COMMIT / ROLLBACK 在 Session 上正确推进 current_txn_id
 *   - 嵌套 BEGIN 报错
 *   - COMMIT/ROLLBACK 在无事务时报错
 *   - 错误语句在事务中将事务标记为 Failed，后续语句被拒绝
 *   - SET TRANSACTION ISOLATION LEVEL 写入 Session.isolation
 */

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

#include "corodb/db/database.h"
#include "corodb/db/session.h"
#include "corodb/storage/lsm_storage_engine.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/txn/transaction_manager.h"

using namespace corodb;

namespace {

    class TempDir {
    public:
        TempDir() {
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            p_ = std::filesystem::temp_directory_path() /
                 ("corodb_txn_test_" + std::to_string(now) + "_" + std::to_string(::rand()));
            std::filesystem::create_directories(p_);
        }
        ~TempDir() {
            try {
                std::filesystem::remove_all(p_);
            } catch (...) {
            }
        }
        std::string path() const {
            return p_.string();
        }

    private:
        std::filesystem::path p_;
    };

    class TxnTest : public ::testing::Test {
    protected:
        std::unique_ptr<TempDir> dir;
        std::unique_ptr<Database> db;
        std::shared_ptr<Session> sess = std::make_shared<Session>();

        void SetUp() override {
            dir = std::make_unique<TempDir>();
            db = std::make_unique<Database>(dir->path());
        }
        void TearDown() override {
            db.reset();
            storage_internal::WalManager::instance().clear_all();
            dir.reset();
        }

        void exec_ok(const std::string& sql) {
            auto r = db->execute(sql, sess);
            if (r.rows.has_value()) {
                for (auto&& rec: *r.rows) {
                    (void)rec;
                }
            }
            EXPECT_TRUE(r.is_success()) << "SQL failed: " << sql;
        }

        void exec_ok_on(std::shared_ptr<Session> s, const std::string& sql) {
            auto r = db->execute(sql, std::move(s));
            if (r.rows.has_value()) {
                for (auto&& rec: *r.rows) {
                    (void)rec;
                }
            }
            EXPECT_TRUE(r.is_success()) << "SQL failed: " << sql;
        }
    };

} // namespace

TEST_F(TxnTest, BeginCommitLifecycle) {
    EXPECT_EQ(sess->current_txn_id, 0u);
    exec_ok("BEGIN");
    EXPECT_NE(sess->current_txn_id, 0u);
    uint64_t id = sess->current_txn_id;
    exec_ok("COMMIT");
    EXPECT_EQ(sess->current_txn_id, 0u);

    exec_ok("BEGIN");
    EXPECT_NE(sess->current_txn_id, 0u);
    EXPECT_NE(sess->current_txn_id, id) << "txn IDs must be unique and monotonic";
}

TEST_F(TxnTest, BeginRollbackLifecycle) {
    exec_ok("BEGIN");
    EXPECT_NE(sess->current_txn_id, 0u);
    exec_ok("ROLLBACK");
    EXPECT_EQ(sess->current_txn_id, 0u);
}

TEST_F(TxnTest, NestedBeginRejected) {
    exec_ok("BEGIN");
    EXPECT_THROW(db->execute("BEGIN", sess), std::runtime_error);
    // 嵌套 BEGIN 失败后事务仍然活跃
    EXPECT_NE(sess->current_txn_id, 0u);
    exec_ok("ROLLBACK");
}

TEST_F(TxnTest, CommitWithoutBeginRejected) {
    EXPECT_THROW(db->execute("COMMIT", sess), std::runtime_error);
}

TEST_F(TxnTest, RollbackWithoutBeginRejected) {
    EXPECT_THROW(db->execute("ROLLBACK", sess), std::runtime_error);
}

TEST_F(TxnTest, ErrorInsideTxnPoisonsTransaction) {
    exec_ok("CREATE TABLE t (id INT64, name TEXT)");
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (1, 'a')");

    // 错误语句（表不存在），应让事务进入 Failed
    EXPECT_THROW(db->execute("INSERT INTO no_such_table VALUES (1, 'x')", sess), std::runtime_error);

    // 后续 DML 应被拒绝（事务已 poisoned）
    EXPECT_THROW(db->execute("INSERT INTO t VALUES (2, 'b')", sess), std::runtime_error);

    // SELECT 也应被拒绝
    EXPECT_THROW(db->execute("SELECT * FROM t", sess), std::runtime_error);

    // COMMIT 失败状态事务必须返回错误并自动 ROLLBACK
    EXPECT_THROW(db->execute("COMMIT", sess), std::runtime_error);
    EXPECT_EQ(sess->current_txn_id, 0u);

    // 现在可以重新开始事务
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (3, 'c')");
    exec_ok("COMMIT");
}

TEST_F(TxnTest, RollbackPoisonedTransaction) {
    exec_ok("CREATE TABLE t (id INT64, name TEXT)");
    exec_ok("BEGIN");
    EXPECT_THROW(db->execute("SELECT * FROM no_such_table", sess), std::runtime_error);
    // 显式 ROLLBACK 必须成功，即便事务已是 Failed
    exec_ok("ROLLBACK");
    EXPECT_EQ(sess->current_txn_id, 0u);
}

TEST_F(TxnTest, SetTransactionIsolationLevel) {
    EXPECT_EQ(sess->isolation, IsolationLevel::ReadCommitted);

    exec_ok("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    EXPECT_EQ(sess->isolation, IsolationLevel::Serializable);

    exec_ok("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    EXPECT_EQ(sess->isolation, IsolationLevel::RepeatableRead);

    exec_ok("SET TRANSACTION ISOLATION LEVEL READ COMMITTED");
    EXPECT_EQ(sess->isolation, IsolationLevel::ReadCommitted);

    exec_ok("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    EXPECT_EQ(sess->isolation, IsolationLevel::ReadUncommitted);
}

TEST_F(TxnTest, SetIsolationInsideTxnRejected) {
    exec_ok("BEGIN");
    EXPECT_THROW(db->execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", sess), std::runtime_error);
    exec_ok("ROLLBACK");
}

TEST_F(TxnTest, IsolationLevelPersistsAcrossTransactions) {
    exec_ok("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    exec_ok("BEGIN");
    EXPECT_EQ(sess->isolation, IsolationLevel::Serializable);
    exec_ok("COMMIT");
    EXPECT_EQ(sess->isolation, IsolationLevel::Serializable);
}

TEST_F(TxnTest, MultipleSessionsHaveIndependentTxnIds) {
    auto s1 = std::make_shared<Session>();
    auto s2 = std::make_shared<Session>();
    auto r1 = db->execute("BEGIN", s1);
    auto r2 = db->execute("BEGIN", s2);
    EXPECT_NE(s1->current_txn_id, 0u);
    EXPECT_NE(s2->current_txn_id, 0u);
    EXPECT_NE(s1->current_txn_id, s2->current_txn_id) << "two sessions must get different txn IDs";
    db->execute("COMMIT", s1);
    db->execute("ROLLBACK", s2);
}

// ============================================================
// 写入路径约束强制（NOT NULL / 类型 / 主键唯一性）
// ============================================================

TEST_F(TxnTest, NotNullConstraintRejectsNullColumn) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL)");
    // 省略 name 列 → 默认 NULL → 触发 NOT NULL 约束。
    bool threw = false;
    try {
        db->execute("INSERT INTO t (id) VALUES (1)", sess);
    } catch (const std::exception& ex) {
        threw = true;
        EXPECT_NE(std::string(ex.what()).find("NOT NULL"), std::string::npos) << ex.what();
    }
    EXPECT_TRUE(threw) << "NULL into NOT NULL column must be rejected";
    // 提供全部非 NULL 列则成功。
    exec_ok("INSERT INTO t VALUES (2, 'ok')");
}

TEST_F(TxnTest, TypeMismatchRejected) {
    exec_ok("CREATE TABLE t (id INT, name TEXT)");
    // 字符串插入 INT 列 → 类型不匹配。
    EXPECT_THROW(db->execute("INSERT INTO t VALUES ('abc', 'x')", sess), std::runtime_error);
    // 正确类型仍可插入。
    exec_ok("INSERT INTO t VALUES (1, 'x')");
}

TEST_F(TxnTest, DuplicatePrimaryKeyRejectedAutoCommit) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    // 重复主键→拒绝。
    EXPECT_THROW(db->execute("INSERT INTO t VALUES (1, 20)", sess), std::runtime_error);
    // 不同主键仍可插入。
    exec_ok("INSERT INTO t VALUES (2, 20)");
}

TEST_F(TxnTest, DuplicatePrimaryKeyRejectedInTxn) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    exec_ok("BEGIN");
    // 事务内插入已提交的主键 → 重复键，事务被 poisoned。
    EXPECT_THROW(db->execute("INSERT INTO t VALUES (1, 99)", sess), std::runtime_error);
    exec_ok("ROLLBACK");
}

// ============================================================
// Phase 2 / T2.2-T2.3 / T3.3: TxnWriteBuffer + 写写冲突
// ============================================================

namespace {
    // 数 SELECT 的行数（必须迭代 generator 才会触发 plan 执行）
    std::size_t count_rows(Database& db, std::shared_ptr<Session> s, const std::string& sql) {
        auto r = db.execute(sql, std::move(s));
        if (!r.rows.has_value())
            return 0;
        std::size_t n = 0;
        for (auto&& rec: *r.rows) {
            (void)rec;
            ++n;
        }
        return n;
    }
} // namespace

class TxnBufferTest : public TxnTest {
protected:
    void SetUp() override {
        TxnTest::SetUp();
        exec_ok("CREATE TABLE t (id INT64, v INT64)");
        exec_ok("INSERT INTO t VALUES (1, 100)");
        exec_ok("INSERT INTO t VALUES (2, 200)");
        exec_ok("INSERT INTO t VALUES (3, 300)");
    }
};

TEST_F(TxnBufferTest, RollbackUndoesInsert) {
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (4, 400)");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 4u) << "read-your-own-writes inside txn";
    exec_ok("ROLLBACK");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u) << "ROLLBACK must undo INSERT";
}

TEST_F(TxnBufferTest, RollbackUndoesUpdate) {
    exec_ok("BEGIN");
    exec_ok("UPDATE t SET v = 999 WHERE id = 1");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 999"), 1u);
    exec_ok("ROLLBACK");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 999"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 100"), 1u);
}

TEST_F(TxnBufferTest, RollbackUndoesDelete) {
    exec_ok("BEGIN");
    exec_ok("DELETE FROM t WHERE id = 2");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 2u);
    exec_ok("ROLLBACK");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u) << "ROLLBACK must undo DELETE";
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 2"), 1u);
}

TEST_F(TxnBufferTest, CommitAppliesAllDml) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (4, 400)");
    exec_ok("UPDATE t SET v = 111 WHERE id = 1");
    exec_ok("DELETE FROM t WHERE id = 3");
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 111"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 3"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 4"), 1u);
}

TEST_F(TxnBufferTest, NoDirtyReadAcrossSessions) {
    auto other = std::make_shared<Session>();
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (4, 400)");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 4u) << "writer reads its own writes";
    EXPECT_EQ(count_rows(*db, other, "SELECT * FROM t"), 3u) << "other session must not see uncommitted INSERT";
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, other, "SELECT * FROM t"), 4u) << "after COMMIT, other session sees the new row";
}

TEST_F(TxnBufferTest, InsertThenUpdateInSameTxnMerges) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (10, 1000)");
    exec_ok("UPDATE t SET v = 1111 WHERE id = 10");
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 10 AND v = 1111"), 1u);
}

TEST_F(TxnBufferTest, InsertThenDeleteInSameTxnLeavesNothing) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO t VALUES (11, 1100)");
    exec_ok("DELETE FROM t WHERE id = 11");
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 11"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
}

TEST_F(TxnBufferTest, DeleteThenInsertSamePkRestores) {
    exec_ok("BEGIN");
    exec_ok("DELETE FROM t WHERE id = 1");
    exec_ok("INSERT INTO t VALUES (1, 12345)");
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1 AND v = 12345"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
}

// ---------- T3.3: 写写冲突（first-committer-wins） ----------

TEST_F(TxnBufferTest, WriteWriteConflictOnUpdate) {
    auto s1 = std::make_shared<Session>(); auto s2 = std::make_shared<Session>();
    db->execute("BEGIN", s1);
    db->execute("BEGIN", s2);

    // s1 先写 id=1
    db->execute("UPDATE t SET v = 11 WHERE id = 1", s1);

    // s2 也想写 id=1 → 必须冲突
    EXPECT_THROW(db->execute("UPDATE t SET v = 22 WHERE id = 1", s2), std::runtime_error);

    // s2 现在是 Failed 事务，必须 ROLLBACK
    EXPECT_THROW(db->execute("COMMIT", s2), std::runtime_error);
    EXPECT_EQ(s2->current_txn_id, 0u);

    // s1 正常 COMMIT
    db->execute("COMMIT", s1);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1 AND v = 11"), 1u);
}

TEST_F(TxnBufferTest, WriteWriteConflictOnInsert) {
    auto s1 = std::make_shared<Session>(); auto s2 = std::make_shared<Session>();
    db->execute("BEGIN", s1);
    db->execute("BEGIN", s2);

    db->execute("INSERT INTO t VALUES (50, 5000)", s1);
    EXPECT_THROW(db->execute("INSERT INTO t VALUES (50, 9999)", s2), std::runtime_error);

    db->execute("ROLLBACK", s2);
    db->execute("COMMIT", s1);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 50 AND v = 5000"), 1u);
}

TEST_F(TxnBufferTest, WriteWriteConflictOnDelete) {
    auto s1 = std::make_shared<Session>(); auto s2 = std::make_shared<Session>();
    db->execute("BEGIN", s1);
    db->execute("BEGIN", s2);

    db->execute("DELETE FROM t WHERE id = 2", s1);
    EXPECT_THROW(db->execute("UPDATE t SET v = 222 WHERE id = 2", s2), std::runtime_error);

    db->execute("ROLLBACK", s2);
    db->execute("COMMIT", s1);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 2"), 0u);
}

TEST_F(TxnBufferTest, RollbackReleasesRowLockForOthers) {
    auto s1 = std::make_shared<Session>(); auto s2 = std::make_shared<Session>();
    db->execute("BEGIN", s1);
    db->execute("UPDATE t SET v = 11 WHERE id = 1", s1);
    db->execute("ROLLBACK", s1); // 释放 (t, 1) 锁

    // s2 现在应能成功更新同一行
    db->execute("BEGIN", s2);
    db->execute("UPDATE t SET v = 22 WHERE id = 1", s2);
    db->execute("COMMIT", s2);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1 AND v = 22"), 1u);
}

TEST_F(TxnBufferTest, CommitReleasesRowLockForOthers) {
    auto s1 = std::make_shared<Session>(); auto s2 = std::make_shared<Session>();
    db->execute("BEGIN", s1);
    db->execute("UPDATE t SET v = 11 WHERE id = 1", s1);
    db->execute("COMMIT", s1); // 释放 (t, 1) 锁

    db->execute("BEGIN", s2);
    db->execute("UPDATE t SET v = 22 WHERE id = 1", s2);
    db->execute("COMMIT", s2);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1 AND v = 22"), 1u);
}

TEST_F(TxnBufferTest, IdempotentLockOnSamePkWithinTxn) {
    // 同一事务对同一 pk 多次写入应幂等，不报冲突
    exec_ok("BEGIN");
    exec_ok("UPDATE t SET v = 10 WHERE id = 1");
    exec_ok("UPDATE t SET v = 11 WHERE id = 1");
    exec_ok("UPDATE t SET v = 12 WHERE id = 1");
    exec_ok("COMMIT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1 AND v = 12"), 1u);
}

// ---------------- MVCC commit_ts 启动恢复（T2.5 基础）----------------

class MvccBootstrapTest : public ::testing::Test {
protected:
    std::unique_ptr<TempDir> dir;

    void SetUp() override {
        dir = std::make_unique<TempDir>();
    }
    void TearDown() override {
        storage_internal::WalManager::instance().clear_all();
        dir.reset();
    }
};

TEST_F(MvccBootstrapTest, MaxObservedCommitTsScansWalAndSstable) {
    // 1) 第一次启动：写入若干事务，记录最后一次分配的 commit_ts
    {
        Database db(dir->path());
        auto s = std::make_shared<Session>();
        ASSERT_TRUE(db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)", s).is_success());
        ASSERT_TRUE(db.execute("BEGIN", s).is_success());
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (1, 10)", s).is_success());
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (2, 20)", s).is_success());
        ASSERT_TRUE(db.execute("COMMIT", s).is_success());
        ASSERT_TRUE(db.execute("BEGIN", s).is_success());
        ASSERT_TRUE(db.execute("UPDATE t SET v = 99 WHERE id = 1", s).is_success());
        ASSERT_TRUE(db.execute("COMMIT", s).is_success());
    }

    // 2) 直接通过引擎接口扫描磁盘观察到的最大 commit_ts，应为 > 0
    {
        auto eng = std::make_unique<LSMTreeEngine>(dir->path());
        const uint64_t max_ts = eng->max_observed_commit_ts();
        EXPECT_GT(max_ts, 0u) << "WAL/SSTable should carry persisted commit_ts";
    }

    // 3) 二次启动：Database 构造时必然 bootstrap，next_ts > 已观察值。
    //    这里通过新事务的 commit 不抛错来侧面验证（具体 ts 内部不可见）。
    {
        Database db(dir->path());
        auto s = std::make_shared<Session>();
        ASSERT_TRUE(db.execute("BEGIN", s).is_success());
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (3, 30)", s).is_success());
        ASSERT_TRUE(db.execute("COMMIT", s).is_success());
        auto r = db.execute("SELECT * FROM t WHERE id = 3", s);
        ASSERT_TRUE(r.is_success());
        ASSERT_TRUE(r.rows.has_value());
        size_t cnt = 0;
        for (auto&& rec: *r.rows) {
            (void)rec;
            ++cnt;
        }
        EXPECT_EQ(cnt, 1u);
    }
}

TEST_F(MvccBootstrapTest, MaxObservedCommitTsEmptyDirReturnsZero) {
    auto eng = std::make_unique<LSMTreeEngine>(dir->path());
    EXPECT_EQ(eng->max_observed_commit_ts(), 0u);
}

// =====================================================================
// MVCC 端到端隔离级别测试（T2.4 + T3.1 + T3.2）
//
// 验证：BEGIN 之后、其它 session 提交的写不影响：
//   - REPEATABLE READ：整事务一个 snapshot，看不到并发提交
//   - READ COMMITTED：每语句一个 snapshot，能看到并发提交
// =====================================================================

class MvccIsolationTest : public TxnTest {
protected:
    void SetUp() override {
        TxnTest::SetUp();
        exec_ok("CREATE TABLE t (id INT64, v INT64)");
        exec_ok("INSERT INTO t VALUES (1, 100)");
        exec_ok("INSERT INTO t VALUES (2, 200)");
    }
};

TEST_F(MvccIsolationTest, RepeatableReadSeesStableSnapshot) {
    auto reader = std::make_shared<Session>();
    auto r1 = db->execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", reader);
    EXPECT_TRUE(r1.is_success());
    exec_ok_on(reader, "BEGIN");
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 2u);

    // 另一会话 auto-commit 插入新行
    auto writer = std::make_shared<Session>();
    auto rw = db->execute("INSERT INTO t VALUES (3, 300)", writer);
    EXPECT_TRUE(rw.is_success());
    // sanity：另一连接（auto-commit）能看到新行
    EXPECT_EQ(count_rows(*db, writer, "SELECT * FROM t"), 3u);

    // RR 事务里的 reader 看到的还应是 BEGIN 时的快照（2 行）
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 2u) << "RR snapshot must remain stable across statements";

    exec_ok_on(reader, "COMMIT");
    // 提交后再读，得到最新快照（3 行）
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 3u);
}

TEST_F(MvccIsolationTest, ReadCommittedSeesConcurrentCommit) {
    auto reader = std::make_shared<Session>();
    auto r1 = db->execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", reader);
    EXPECT_TRUE(r1.is_success());
    exec_ok_on(reader, "BEGIN");
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 2u);

    auto writer = std::make_shared<Session>();
    auto rw = db->execute("INSERT INTO t VALUES (3, 300)", writer);
    EXPECT_TRUE(rw.is_success());

    // RC 每语句新 snapshot —— 应看到并发 INSERT
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 3u) << "RC must take fresh snapshot per statement";

    exec_ok_on(reader, "COMMIT");
}

TEST_F(MvccIsolationTest, RepeatableReadIgnoresConcurrentDeleteAndUpdate) {
    auto reader = std::make_shared<Session>();
    db->execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", reader);
    exec_ok_on(reader, "BEGIN");
    auto initial = count_rows(*db, reader, "SELECT * FROM t WHERE v = 100");
    EXPECT_EQ(initial, 1u);

    auto writer = std::make_shared<Session>();
    db->execute("UPDATE t SET v = 999 WHERE id = 1", writer);
    db->execute("DELETE FROM t WHERE id = 2", writer);

    // RR 仍然看到旧值
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t WHERE v = 100"), 1u) << "RR ignores concurrent UPDATE";
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t WHERE id = 2"), 1u) << "RR ignores concurrent DELETE";
    EXPECT_EQ(count_rows(*db, reader, "SELECT * FROM t"), 2u);

    exec_ok_on(reader, "COMMIT");
}

TEST_F(MvccIsolationTest, AutoCommitInsertVisibleToFreshSelect) {
    // 验证 auto-commit 路径下 SELECT 走 MVCC 仍正常
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 2u);
    exec_ok("INSERT INTO t VALUES (5, 500)");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
    exec_ok("DELETE FROM t WHERE id = 1");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 2u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 1"), 0u);
}

// =====================================================================
// SSI（Serializable Snapshot Isolation）测试（T3.4）
// =====================================================================

TEST_F(MvccIsolationTest, SerializableAbortsOnReadSetConflict) {
    // 经典 write-skew：A 在 SER 下读 row 1，期间 B auto-commit 改写 row 1，A 提交时应失败
    auto a = std::make_shared<Session>();
    EXPECT_TRUE(db->execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", a).is_success());
    exec_ok_on(a, "BEGIN");
    // A 读 row 1（进入 read_set）
    EXPECT_EQ(count_rows(*db, a, "SELECT * FROM t WHERE id = 1"), 1u);

    // 另一会话改写 row 1 并提交
    auto b = std::make_shared<Session>();
    EXPECT_TRUE(db->execute("UPDATE t SET v = 999 WHERE id = 1", b).is_success());

    // A 再做点写（确保走 commit 验证路径）
    EXPECT_TRUE(db->execute("INSERT INTO t VALUES (10, 10)", a).is_success());

    // A 提交应失败（抛异常或返回失败）
    bool failed = false;
    try {
        auto cr = db->execute("COMMIT", a);
        failed = !cr.is_success();
    } catch (const std::exception&) {
        failed = true;
    }
    EXPECT_TRUE(failed) << "SER commit should fail when read-set row was modified by concurrent committer";
}

TEST_F(MvccIsolationTest, SerializableCommitsWithoutConflict) {
    auto a = std::make_shared<Session>();
    EXPECT_TRUE(db->execute("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", a).is_success());
    exec_ok_on(a, "BEGIN");
    EXPECT_EQ(count_rows(*db, a, "SELECT * FROM t WHERE id = 1"), 1u);
    EXPECT_TRUE(db->execute("INSERT INTO t VALUES (11, 11)", a).is_success());
    auto cr = db->execute("COMMIT", a);
    EXPECT_TRUE(cr.is_success());
}

TEST_F(MvccIsolationTest, RepeatableReadDoesNotValidateReadSet) {
    // RR 下不做 SSI 验证，同样的 write-skew 模式应能成功 commit
    auto a = std::make_shared<Session>();
    EXPECT_TRUE(db->execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", a).is_success());
    exec_ok_on(a, "BEGIN");
    EXPECT_EQ(count_rows(*db, a, "SELECT * FROM t WHERE id = 1"), 1u);

    auto b = std::make_shared<Session>();
    EXPECT_TRUE(db->execute("UPDATE t SET v = 999 WHERE id = 1", b).is_success());

    EXPECT_TRUE(db->execute("INSERT INTO t VALUES (12, 12)", a).is_success());
    auto cr = db->execute("COMMIT", a);
    EXPECT_TRUE(cr.is_success()) << "RR must not validate read-set";
}

// =====================================================================
// 二级索引按主键（value→pk）+ IndexScan 可见性重查
// =====================================================================

TEST_F(TxnTest, IndexScanReturnsCorrectRows) {
    exec_ok("CREATE TABLE t (id INT, city TEXT)");
    exec_ok("CREATE INDEX idx_city ON t (city)");
    exec_ok("INSERT INTO t VALUES (1, 'NYC')");
    exec_ok("INSERT INTO t VALUES (2, 'LA')");
    exec_ok("INSERT INTO t VALUES (3, 'NYC')");
    // 等值查询走 IndexScan（value→pk）。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'NYC'"), 2u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'LA'"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'SF'"), 0u);
}

TEST_F(TxnTest, IndexScanReflectsUpdatesViaRecheck) {
    exec_ok("CREATE TABLE t (id INT, city TEXT)");
    exec_ok("CREATE INDEX idx_city ON t (city)");
    exec_ok("INSERT INTO t VALUES (1, 'NYC')");
    exec_ok("UPDATE t SET city = 'LA' WHERE id = 1");
    // 超集索引仍含陈旧的 (NYC→1)，但可见性重查过滤：查 NYC 应 0 行，查 LA 应 1 行。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'NYC'"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'LA'"), 1u);
}

TEST_F(TxnTest, IndexScanAfterDeleteFiltersTombstone) {
    exec_ok("CREATE TABLE t (id INT, city TEXT)");
    exec_ok("CREATE INDEX idx_city ON t (city)");
    exec_ok("INSERT INTO t VALUES (1, 'NYC')");
    exec_ok("INSERT INTO t VALUES (2, 'NYC')");
    exec_ok("DELETE FROM t WHERE id = 1");
    // 删除后索引仍含 (NYC→1)，但 lookup_visible 返回 tombstone → 过滤掉，应剩 1 行。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'NYC'"), 1u);
}

TEST_F(TxnTest, IndexSurvivesRestart) {
    exec_ok("CREATE TABLE t (id INT, city TEXT)");
    exec_ok("CREATE INDEX idx_city ON t (city)");
    exec_ok("INSERT INTO t VALUES (1, 'NYC')");
    exec_ok("INSERT INTO t VALUES (2, 'NYC')");
    // 重启：销毁并在同目录重建 Database，索引从 .idx 文件恢复。
    db.reset();
    storage_internal::WalManager::instance().clear_all();
    db = std::make_unique<Database>(dir->path());
    sess = std::make_shared<Session>();
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE city = 'NYC'"), 2u);
}

TEST_F(TxnTest, RangeIndexScanReturnsCorrectRows) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    exec_ok("INSERT INTO t VALUES (2, 30)");
    exec_ok("INSERT INTO t VALUES (3, 40)");
    exec_ok("INSERT INTO t VALUES (4, 50)");
    // 范围条件走 range IndexScan（有序 value→pk）。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age > 30"), 2u);  // 40,50
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age >= 30"), 3u); // 30,40,50
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age < 40"), 2u);  // 20,30
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age <= 40"), 3u); // 20,30,40
}

TEST_F(TxnTest, RangeIndexScanReflectsUpdatesAndDeletes) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    exec_ok("INSERT INTO t VALUES (2, 30)");
    exec_ok("UPDATE t SET age = 99 WHERE id = 1"); // 20 → 99
    exec_ok("DELETE FROM t WHERE id = 2");         // 删 30
    // 超集索引仍含 (20→1),(30→2)，但可见性重查过滤：age>50 应只剩 id=1(99)。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age > 50"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age < 50"), 0u);
}

TEST_F(TxnTest, RangePredicateUsesIndexScan) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    // EXPLAIN 应显示 range IndexScan（而非 Seq Scan）。
    auto r = db->execute("EXPLAIN SELECT * FROM t WHERE age > 10", sess);
    ASSERT_TRUE(r.rows.has_value());
    std::string plan;
    for (auto&& rec: *r.rows)
        for (const auto& v: rec.values)
            if (std::holds_alternative<std::string>(v))
                plan += std::get<std::string>(v) + "\n";
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("age > 10"), std::string::npos) << plan;
}

TEST_F(TxnTest, BetweenUsesRangeIndexScan) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    exec_ok("INSERT INTO t VALUES (2, 30)");
    exec_ok("INSERT INTO t VALUES (3, 40)");
    exec_ok("INSERT INTO t VALUES (4, 50)");
    // BETWEEN 双侧含等 → range IndexScan：30,40 命中。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age BETWEEN 30 AND 40"), 2u);
    // 确认走 IndexScan。
    auto r = db->execute("EXPLAIN SELECT * FROM t WHERE age BETWEEN 25 AND 45", sess);
    ASSERT_TRUE(r.rows.has_value());
    std::string plan;
    for (auto&& rec: *r.rows)
        for (const auto& v: rec.values)
            if (std::holds_alternative<std::string>(v))
                plan += std::get<std::string>(v) + "\n";
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
}

TEST_F(TxnTest, StringPrimaryKeyCrud) {
    exec_ok("CREATE TABLE users (id TEXT, name TEXT)");
    exec_ok("INSERT INTO users VALUES ('alice', 'Alice')");
    exec_ok("INSERT INTO users VALUES ('bob', 'Bob')");
    exec_ok("INSERT INTO users VALUES ('carol', 'Carol')");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users"), 3u);
    // 重复字符串主键应被拒绝（lookup_visible 按字符串主键点查）。
    EXPECT_THROW(db->execute("INSERT INTO users VALUES ('alice', 'Dup')", sess), std::runtime_error);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users"), 3u);
    // 按字符串主键 UPDATE / DELETE。
    exec_ok("UPDATE users SET name = 'Bobby' WHERE id = 'bob'");
    exec_ok("DELETE FROM users WHERE id = 'carol'");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users WHERE name = 'Bobby'"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users WHERE id = 'carol'"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users"), 2u);
}

TEST_F(TxnTest, StringPrimaryKeyIndexSurvivesRestart) {
    exec_ok("CREATE TABLE users (id TEXT, city TEXT)");
    exec_ok("CREATE INDEX idx_city ON users (city)");
    exec_ok("INSERT INTO users VALUES ('u1', 'NYC')");
    exec_ok("INSERT INTO users VALUES ('u2', 'LA')");
    exec_ok("INSERT INTO users VALUES ('u3', 'NYC')");
    exec_ok("CHECKPOINT");
    // 重启：从磁盘重新加载（含字符串主键的 SSTable + 索引文件，encode/decode_key 往返）。
    db = std::make_unique<Database>(dir->path());
    sess = std::make_shared<Session>();
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users WHERE city = 'NYC'"), 2u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM users"), 3u);
}

TEST_F(TxnTest, StringPrimaryKeyTransactionIsolation) {
    exec_ok("CREATE TABLE kv (k TEXT, v INT)");
    exec_ok("INSERT INTO kv VALUES ('x', 1)");
    exec_ok("BEGIN");
    exec_ok("UPDATE kv SET v = 99 WHERE k = 'x'");
    exec_ok("INSERT INTO kv VALUES ('y', 2)");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM kv WHERE v = 99"), 1u) << "read-your-own-writes (string pk)";
    exec_ok("ROLLBACK");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM kv WHERE v = 99"), 0u) << "ROLLBACK must undo string-pk update";
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM kv"), 1u) << "ROLLBACK must undo string-pk insert";
}

TEST_F(TxnTest, InListUsesIndexScan) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    exec_ok("INSERT INTO t VALUES (2, 30)");
    exec_ok("INSERT INTO t VALUES (3, 40)");
    exec_ok("INSERT INTO t VALUES (4, 50)");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age IN (20, 40)"), 2u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age IN (99)"), 0u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age IN (30, 40, 50)"), 3u);
    // 确认走 IndexScan（含 IN 条件）。
    auto r = db->execute("EXPLAIN SELECT * FROM t WHERE age IN (20, 40)", sess);
    ASSERT_TRUE(r.rows.has_value());
    std::string plan;
    for (auto&& rec: *r.rows)
        for (const auto& v: rec.values)
            if (std::holds_alternative<std::string>(v))
                plan += std::get<std::string>(v) + "\n";
    EXPECT_NE(plan.find("Index Scan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("IN ("), std::string::npos) << plan;
}

TEST_F(TxnTest, InListIndexReflectsUpdatesAndDeletes) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("CREATE INDEX idx_age ON t (age)");
    exec_ok("INSERT INTO t VALUES (1, 20)");
    exec_ok("INSERT INTO t VALUES (2, 30)");
    exec_ok("INSERT INTO t VALUES (3, 40)");
    exec_ok("UPDATE t SET age = 99 WHERE id = 1"); // 20 → 99（超集索引仍含 20→1）
    exec_ok("DELETE FROM t WHERE id = 2");         // 删 30
    // IN (20, 30, 99)：20 已改、3 0 已删，仅 id=1(99) 命中；可见性重查过滤陈旧超集条目。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age IN (20, 30, 99)"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age IN (20, 30)"), 0u);
}

TEST_F(TxnTest, CheckpointThenReadInProcess) {
    exec_ok("CREATE TABLE t (id INT, age INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    for (int a = 11; a <= 20; ++a)
        exec_ok("UPDATE t SET age = " + std::to_string(a) + " WHERE id = 1");
    exec_ok("CHECKPOINT");
    // CHECKPOINT 后同一进程内读取：Buffer Pool 旧页帧必须已失效（否则命中陈旧页读到 0 行）。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age = 20"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE age = 10"), 0u);
    // 多行 + 删除，覆盖 flush/compaction 后再读。
    exec_ok("INSERT INTO t VALUES (2, 200)");
    exec_ok("INSERT INTO t VALUES (3, 300)");
    exec_ok("CHECKPOINT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);
    exec_ok("DELETE FROM t WHERE id = 2");
    exec_ok("CHECKPOINT");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 2u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 2"), 0u);
}

TEST_F(TxnTest, StorageBackedTableDoesNotMirrorRowsInMemory) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    for (int i = 1; i <= 50; ++i)
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
    // 数据可查询（经 scan_visible 流式获取），但 rows_ 不再全量常驻（去内存天花板）。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 50u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 250"), 1u);
    auto tbl = db->get_catalog().lookup("t");
    ASSERT_NE(tbl, nullptr);
    EXPECT_TRUE(tbl->has_storage());
    EXPECT_TRUE(tbl->rows().empty()) << "storage-backed table must not keep full rows_ in memory";
    // 更新/删除后仍正确，且 rows_ 仍为空。
    exec_ok("UPDATE t SET v = 999 WHERE id = 1");
    exec_ok("DELETE FROM t WHERE id = 2");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 999"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 49u);
    EXPECT_TRUE(tbl->rows().empty()) << "writes must not repopulate rows_ for a storage-backed table";
}

TEST_F(TxnTest, StreamingScanMergesMemtableAndSSTable) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    exec_ok("INSERT INTO t VALUES (2, 20)");
    exec_ok("INSERT INTO t VALUES (3, 30)");
    exec_ok("CHECKPOINT");                        // flush 到 SSTable
    exec_ok("UPDATE t SET v = 999 WHERE id = 2"); // 新版本进 memtable（需覆盖 SSTable 版本）
    exec_ok("INSERT INTO t VALUES (4, 40)");      // 仅在 memtable
    exec_ok("DELETE FROM t WHERE id = 3");        // tombstone 进 memtable（需覆盖 SSTable）
    // 流式 k 路归并：memtable 版本按 ts 覆盖 SSTable；tombstone 跨源过滤；memtable-only 行包含。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 3u);                // 1,2,4（3 已删）
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 999"), 1u); // id=2 memtable 版本
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE v = 20"), 0u);  // 旧 SSTable 版本不可见
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 3"), 0u);  // tombstone
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 4"), 1u);  // 仅 memtable
}

TEST_F(TxnTest, StreamingScanAcrossManySSTablePages) {
    exec_ok("CREATE TABLE t (id INT, name TEXT)");
    for (int i = 1; i <= 500; ++i)
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row-" + std::to_string(i) + "-padding-xxxxxxxx')");
    exec_ok("CHECKPOINT"); // flush 到 SSTable，数据跨多页
    // 流式扫描逐页读取（经 Buffer Pool pin 页，记录跨页正确拼接）。
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 500u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 250"), 1u);
    // 更新一条（进 memtable）+ 删一条，再全表流式读，验证跨源归并 + 跨页。
    exec_ok("UPDATE t SET name = 'updated' WHERE id = 1");
    exec_ok("DELETE FROM t WHERE id = 500");
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 499u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE name = 'updated'"), 1u);
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t WHERE id = 500"), 0u);
}

TEST_F(TxnTest, WalRecoveryToleratesTornOrCorruptTail) {
    exec_ok("CREATE TABLE t (id INT, v INT)");
    exec_ok("INSERT INTO t VALUES (1, 10)");
    exec_ok("INSERT INTO t VALUES (2, 20)");
    // 关闭 DB，模拟崩溃时 WAL 尾部的撞裂/损坏写入（合法头 + 巨大且损坏的 len）。
    db.reset();
    storage_internal::WalManager::instance().clear_all();
    std::filesystem::path wal = std::filesystem::path(dir->path()) / "t.wal";
    ASSERT_TRUE(std::filesystem::exists(wal));
    {
        std::ofstream ofs(wal, std::ios::binary | std::ios::app);
        uint8_t type = 11;
        uint32_t len = 0xFFFFFFF0u; // 巨大且损坏的长度：修复前会尝试分配 ~4GB 而 OOM/崩溃
        uint32_t checksum = 12345u;
        ofs.write(reinterpret_cast<char*>(&type), sizeof(type));
        ofs.write(reinterpret_cast<char*>(&len), sizeof(len));
        ofs.write(reinterpret_cast<char*>(&checksum), sizeof(checksum));
        ofs.write("garbage", 7); // 远少于 len 声称的字节
    }
    // 重启：恢复必须读到前 2 行，安全忽略撞裂尾部（不 OOM、不崩溃）。
    db = std::make_unique<Database>(dir->path());
    sess = std::make_shared<Session>();
    EXPECT_EQ(count_rows(*db, sess, "SELECT * FROM t"), 2u);
}

// =====================================================================
// T9.5.1: TransactionManager::min_active_read_ts 单测
// =====================================================================

TEST(TransactionManagerGcHorizon, NoActiveTxnReturnsNextTs) {
    TransactionManager tm;
    const auto a = tm.min_active_read_ts();
    // 无活跃事务时 horizon = next_ts_，新 begin() 后 horizon 单调推进
    auto id = tm.begin();
    EXPECT_GE(*tm.get_read_ts(id), a);
    tm.commit(id);
    const auto b = tm.min_active_read_ts();
    EXPECT_GE(b, a) << "horizon must advance after txn finishes";
}

TEST(TransactionManagerGcHorizon, SingleActiveReturnsThatReadTs) {
    TransactionManager tm;
    auto id = tm.begin();
    auto rt = *tm.get_read_ts(id);
    EXPECT_EQ(tm.min_active_read_ts(), rt);
    tm.commit(id);
}

TEST(TransactionManagerGcHorizon, MultiActiveReturnsMin) {
    TransactionManager tm;
    auto a = tm.begin();
    auto b = tm.begin();
    auto c = tm.begin();
    auto rt_a = *tm.get_read_ts(a);
    EXPECT_EQ(tm.min_active_read_ts(), rt_a);
    tm.commit(a);
    EXPECT_EQ(tm.min_active_read_ts(), *tm.get_read_ts(b));
    tm.commit(b);
    EXPECT_EQ(tm.min_active_read_ts(), *tm.get_read_ts(c));
    tm.commit(c);
}


