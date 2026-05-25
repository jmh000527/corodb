/**
 * @file test_storage.cpp
 * @brief 存储模块单元测试
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 测试存储引擎、表、目录和缓冲池等组件
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <memory>

#include "corodb/common/types.h"
#include "corodb/storage/buffer_pool.h"
#include "corodb/storage/lsm_storage_engine.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/storage/table.h"

using namespace corodb;

// ============================================================================
// 测试辅助工具
// ============================================================================

class TempDirectory {
public:
    TempDirectory(const std::string& prefix = "corodb_test") {
        // 使用随机数生成器创建唯一目录名
        static std::atomic<int> counter{ 0 };
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        auto id = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "_" + std::to_string(now) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        try {
            std::filesystem::remove_all(path_);
        } catch (...) {
            // 忽略清理错误
        }
    }

    std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

// ============================================================================
// Column 测试
// ============================================================================

class ColumnTest : public ::testing::Test {};

TEST_F(ColumnTest, DefaultConstruction) {
    Column col;
    col.table = "users";
    col.name = "id";
    col.type = TypeKind::Int64;

    EXPECT_EQ(col.qualified_name(), "users.id");
}

TEST_F(ColumnTest, QualifiedName) {
    Column col{ "users", "name", TypeKind::Text, 1, 1 };
    EXPECT_EQ(col.qualified_name(), "users.name");
}

TEST_F(ColumnTest, QualifiedNameWithoutTable) {
    Column col{ "", "name", TypeKind::Text, 1, 0 };
    EXPECT_EQ(col.qualified_name(), "name");
}

TEST_F(ColumnTest, HasValidOid) {
    Column col1{ "t", "c", TypeKind::Int64, 0, 0 };
    Column col2{ "t", "c", TypeKind::Int64, 1, 0 };

    EXPECT_FALSE(col1.has_valid_oid());
    EXPECT_TRUE(col2.has_valid_oid());
}

// ============================================================================
// Row 测试
// ============================================================================

class RowTest : public ::testing::Test {};

TEST_F(RowTest, EmptyRow) {
    Row row;
    EXPECT_TRUE(row.values.empty());
}

TEST_F(RowTest, RowWithValues) {
    Row row;
    row.values.push_back(int64_t{ 1 });
    row.values.push_back(std::string{ "Alice" });
    row.values.push_back(NullValue{});

    EXPECT_EQ(row.values.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(row.values[0]), 1);
    EXPECT_EQ(std::get<std::string>(row.values[1]), "Alice");
    EXPECT_TRUE(std::holds_alternative<NullValue>(row.values[2]));
}

// ============================================================================
// ValueHash 和 ValueEq 测试
// ============================================================================

class ValueHashTest : public ::testing::Test {
protected:
    ValueHash hasher;
    ValueEq eq;
};

TEST_F(ValueHashTest, HashNullValue) {
    Value v1 = NullValue{};
    Value v2 = NullValue{};

    // 相同值应该有相同的哈希
    EXPECT_EQ(hasher(v1), hasher(v2));
    EXPECT_TRUE(eq(v1, v2));
}

TEST_F(ValueHashTest, HashIntValue) {
    Value v1 = int64_t{ 42 };
    Value v2 = int64_t{ 42 };
    Value v3 = int64_t{ 43 };

    EXPECT_EQ(hasher(v1), hasher(v2));
    EXPECT_TRUE(eq(v1, v2));
    EXPECT_FALSE(eq(v1, v3));
}

TEST_F(ValueHashTest, HashStringValue) {
    Value v1 = std::string{ "hello" };
    Value v2 = std::string{ "hello" };
    Value v3 = std::string{ "world" };

    EXPECT_EQ(hasher(v1), hasher(v2));
    EXPECT_TRUE(eq(v1, v2));
    EXPECT_FALSE(eq(v1, v3));
}

TEST_F(ValueHashTest, DifferentTypesNotEqual) {
    Value null_v = NullValue{};
    Value int_v = int64_t{ 0 };
    Value str_v = std::string{ "" };

    EXPECT_FALSE(eq(null_v, int_v));
    EXPECT_FALSE(eq(int_v, str_v));
    EXPECT_FALSE(eq(null_v, str_v));
}

// ============================================================================
// PageHeader 测试
// ============================================================================

class PageHeaderTest : public ::testing::Test {};

TEST_F(PageHeaderTest, DefaultConstruction) {
    PageHeader header;

    EXPECT_EQ(header.lsn, 0u);
    EXPECT_EQ(header.checksum, 0u);
    EXPECT_EQ(header.slot_count, 0u);
    EXPECT_EQ(header.free_start, 0u);
    EXPECT_EQ(header.free_end, 0u);
}

TEST_F(PageHeaderTest, FreeSpace) {
    PageHeader header;
    header.free_start = 100;
    header.free_end = 500;

    EXPECT_EQ(header.free_space(), 400u);
}

TEST_F(PageHeaderTest, FreeSpaceEmpty) {
    PageHeader header;
    header.free_start = 500;
    header.free_end = 100;

    EXPECT_EQ(header.free_space(), 0u);
}

// ============================================================================
// PageId 测试
// ============================================================================

class PageIdTest : public ::testing::Test {};

TEST_F(PageIdTest, DefaultConstruction) {
    PageId pid;

    EXPECT_TRUE(pid.file.empty());
    EXPECT_EQ(pid.page_idx, 0u);
    EXPECT_FALSE(pid.is_valid());
}

TEST_F(PageIdTest, Construction) {
    PageId pid{ "./data/users.tbl", 5 };

    EXPECT_EQ(pid.file, "./data/users.tbl");
    EXPECT_EQ(pid.page_idx, 5u);
    EXPECT_TRUE(pid.is_valid());
}

TEST_F(PageIdTest, Equality) {
    PageId pid1{ "./data/users.tbl", 0 };
    PageId pid2{ "./data/users.tbl", 0 };
    PageId pid3{ "./data/users.tbl", 1 };
    PageId pid4{ "./data/orders.tbl", 0 };

    EXPECT_EQ(pid1, pid2);
    EXPECT_NE(pid1, pid3);
    EXPECT_NE(pid1, pid4);
}

// ============================================================================
// Catalog 测试
// ============================================================================

class CatalogTest : public ::testing::Test {
protected:
    Catalog catalog;
};

TEST_F(CatalogTest, EmptyCatalog) {
    EXPECT_EQ(catalog.lookup("nonexistent"), nullptr);
    EXPECT_TRUE(catalog.empty());
}

TEST_F(CatalogTest, RegisterAndFindTable) {
    std::vector<Column> columns = { Column{ "users", "id", TypeKind::Int64, 1, 1 },
                                    Column{ "users", "name", TypeKind::Text, 2, 1 } };

    auto table = std::make_shared<Table>("users", columns);

    catalog.register_table(table);

    auto found = catalog.lookup("users");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name(), "users");
}

TEST_F(CatalogTest, FindByOid) {
    std::vector<Column> columns = { Column{ "orders", "id", TypeKind::Int64, 1, 1 } };

    auto table = std::make_shared<Table>("orders", columns);
    Oid table_oid = table->oid();

    catalog.register_table(table);

    auto found = catalog.lookup(table_oid);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name(), "orders");
}

TEST_F(CatalogTest, MultipleTablesLookup) {
    std::vector<Column> user_cols = { Column{ "users", "id", TypeKind::Int64, 1, 1 } };
    auto users = std::make_shared<Table>("users", user_cols);

    std::vector<Column> order_cols = { Column{ "orders", "id", TypeKind::Int64, 1, 2 } };
    auto orders = std::make_shared<Table>("orders", order_cols);

    catalog.register_table(users);
    catalog.register_table(orders);

    EXPECT_NE(catalog.lookup("users"), nullptr);
    EXPECT_NE(catalog.lookup("orders"), nullptr);
    EXPECT_EQ(catalog.lookup("products"), nullptr);
    EXPECT_EQ(catalog.size(), 2u);
}

// ============================================================================
// LSM 存储引擎测试
// ============================================================================

class LSMEngineTest : public ::testing::Test {
protected:
    std::unique_ptr<TempDirectory> temp_dir;
    std::unique_ptr<StorageEngine> engine;

    void SetUp() override {
        // 清理上一个测试可能残留的 WAL 文件句柄
        storage_internal::WalManager::instance().clear_all();
        temp_dir = std::make_unique<TempDirectory>();
        engine = std::make_unique<LSMTreeEngine>(temp_dir->path());
    }

    void TearDown() override {
        engine.reset();
        // 清理 WalManager 中的所有文件句柄，确保 WAL 文件可以被删除
        storage_internal::WalManager::instance().clear_all();
        temp_dir.reset();
    }
};

TEST_F(LSMEngineTest, CreateTable) {
    std::vector<Column> columns = { Column{ "lsm_test", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "lsm_test", "value", TypeKind::Text, 0, 0 } };

    engine->create_table("lsm_test", columns);
    EXPECT_TRUE(engine->table_exists("lsm_test"));
}

TEST_F(LSMEngineTest, InsertAndScan) {
    std::vector<Column> columns = { Column{ "lsm_test", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "lsm_test", "msg", TypeKind::Text, 0, 0 } };

    engine->create_table("lsm_test", columns);

    Row row;
    row.values = { int64_t{ 100 }, std::string{ "Hello LSM" } };
    engine->append_row("lsm_test", columns, row);

    auto rows = engine->load_rows("lsm_test", columns);
    EXPECT_GE(rows.size(), 1u);
}

// ============================================================================
// MVCC 多版本读路径测试 (T7.2)
// ============================================================================

TEST_F(LSMEngineTest, MvccMultiVersionVisibilityByTs) {
    std::vector<Column> columns = { Column{ "mvcc1", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "mvcc1", "v", TypeKind::Int64, 0, 0 } };
    engine->create_table("mvcc1", columns);

    // ts=10: insert (1, 100)
    Row r1;
    r1.values = { int64_t{ 1 }, int64_t{ 100 } };
    engine->append_row("mvcc1", columns, r1, /*commit_ts=*/10);

    // ts=20: insert (1, 200)（视为同 pk 新版本）
    Row r2;
    r2.values = { int64_t{ 1 }, int64_t{ 200 } };
    engine->append_row("mvcc1", columns, r2, /*commit_ts=*/20);

    // snapshot_ts = 5：尚无版本可见
    EXPECT_FALSE(engine->lookup_visible("mvcc1", columns, 1, 5).has_value());

    // snapshot_ts = 10：仅看到 v=100
    auto v10 = engine->lookup_visible("mvcc1", columns, 1, 10);
    ASSERT_TRUE(v10.has_value());
    EXPECT_EQ(std::get<int64_t>(v10->values[1]), 100);

    // snapshot_ts = 15：仍是 v=100（20 > 15 不可见）
    auto v15 = engine->lookup_visible("mvcc1", columns, 1, 15);
    ASSERT_TRUE(v15.has_value());
    EXPECT_EQ(std::get<int64_t>(v15->values[1]), 100);

    // snapshot_ts = 20：看到 v=200
    auto v20 = engine->lookup_visible("mvcc1", columns, 1, 20);
    ASSERT_TRUE(v20.has_value());
    EXPECT_EQ(std::get<int64_t>(v20->values[1]), 200);

    // snapshot_ts = max：依然 v=200
    auto vmax = engine->lookup_visible("mvcc1", columns, 1, std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(vmax.has_value());
    EXPECT_EQ(std::get<int64_t>(vmax->values[1]), 200);
}

TEST_F(LSMEngineTest, MvccTombstoneShadowsOlderVersionAtSnapshot) {
    std::vector<Column> columns = { Column{ "mvcc2", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "mvcc2", "v", TypeKind::Int64, 0, 0 } };
    engine->create_table("mvcc2", columns);

    Row r;
    r.values = { int64_t{ 42 }, int64_t{ 777 } };
    engine->append_row("mvcc2", columns, r, /*commit_ts=*/10);

    // ts=20 删除
    engine->delete_row_by_key("mvcc2", columns, 42, /*commit_ts=*/20);

    // snapshot=10 仍可见
    auto at10 = engine->lookup_visible("mvcc2", columns, 42, 10);
    ASSERT_TRUE(at10.has_value());
    EXPECT_EQ(std::get<int64_t>(at10->values[1]), 777);

    // snapshot=15 仍可见（删除尚未发生）
    auto at15 = engine->lookup_visible("mvcc2", columns, 42, 15);
    ASSERT_TRUE(at15.has_value());

    // snapshot=20 不可见（被 tombstone 遮蔽）
    EXPECT_FALSE(engine->lookup_visible("mvcc2", columns, 42, 20).has_value());

    // ts=30 重插 (42, 999)
    Row r2;
    r2.values = { int64_t{ 42 }, int64_t{ 999 } };
    engine->append_row("mvcc2", columns, r2, /*commit_ts=*/30);

    // snapshot=25 不可见（tombstone 仍最新）
    EXPECT_FALSE(engine->lookup_visible("mvcc2", columns, 42, 25).has_value());

    // snapshot=30 可见 v=999
    auto at30 = engine->lookup_visible("mvcc2", columns, 42, 30);
    ASSERT_TRUE(at30.has_value());
    EXPECT_EQ(std::get<int64_t>(at30->values[1]), 999);
}

TEST_F(LSMEngineTest, MvccScanVisibleSnapshotConsistency) {
    std::vector<Column> columns = { Column{ "mvcc3", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "mvcc3", "v", TypeKind::Int64, 0, 0 } };
    engine->create_table("mvcc3", columns);

    // pk=1 ts10 v=10; ts20 v=20
    Row a1;
    a1.values = { int64_t{ 1 }, int64_t{ 10 } };
    engine->append_row("mvcc3", columns, a1, 10);
    Row a2;
    a2.values = { int64_t{ 1 }, int64_t{ 20 } };
    engine->append_row("mvcc3", columns, a2, 20);

    // pk=2 ts15 v=150
    Row b1;
    b1.values = { int64_t{ 2 }, int64_t{ 150 } };
    engine->append_row("mvcc3", columns, b1, 15);

    // pk=3 ts25 v=300, ts30 deleted
    Row c1;
    c1.values = { int64_t{ 3 }, int64_t{ 300 } };
    engine->append_row("mvcc3", columns, c1, 25);
    engine->delete_row_by_key("mvcc3", columns, 3, 30);

    auto rows15 = engine->scan_visible("mvcc3", columns, 15);
    // 在 ts=15 应见到 pk=1 (v=10) 与 pk=2 (v=150)；pk=3 还没出生
    EXPECT_EQ(rows15.size(), 2u);

    auto rows25 = engine->scan_visible("mvcc3", columns, 25);
    // ts=25：pk=1 (v=20), pk=2 (v=150), pk=3 (v=300)
    EXPECT_EQ(rows25.size(), 3u);

    auto rows35 = engine->scan_visible("mvcc3", columns, 35);
    // ts=35：pk=1 (v=20), pk=2 (v=150)；pk=3 已被删除
    EXPECT_EQ(rows35.size(), 2u);
}

TEST_F(LSMEngineTest, MvccVersionsSurviveFlushAndRecovery) {
    std::vector<Column> columns = { Column{ "mvcc4", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "mvcc4", "v", TypeKind::Int64, 0, 0 } };
    engine->create_table("mvcc4", columns);

    // 写入足够多条目以触发 flush 到 L0（memtable 限制 64KB），
    // 同时为 pk=7 写两个版本以验证 flush 后仍保留两版本。
    Row k7a;
    k7a.values = { int64_t{ 7 }, int64_t{ 700 } };
    engine->append_row("mvcc4", columns, k7a, 100);
    Row k7b;
    k7b.values = { int64_t{ 7 }, int64_t{ 777 } };
    engine->append_row("mvcc4", columns, k7b, 200);

    for (int64_t i = 0; i < 2000; ++i) {
        Row r;
        r.values = { int64_t{ 1000 + i }, int64_t{ i } };
        engine->append_row("mvcc4", columns, r, 50);
    }

    // 现在 pk=7 的两个版本应该已经下到 L0 SSTable
    auto at100 = engine->lookup_visible("mvcc4", columns, 7, 100);
    ASSERT_TRUE(at100.has_value());
    EXPECT_EQ(std::get<int64_t>(at100->values[1]), 700);

    auto at200 = engine->lookup_visible("mvcc4", columns, 7, 200);
    ASSERT_TRUE(at200.has_value());
    EXPECT_EQ(std::get<int64_t>(at200->values[1]), 777);

    // 模拟重启：销毁 engine 重建，从磁盘恢复
    auto dir = temp_dir->path();
    engine.reset();
    storage_internal::WalManager::instance().clear_all();
    engine = std::make_unique<LSMTreeEngine>(dir);

    auto after_restart_100 = engine->lookup_visible("mvcc4", columns, 7, 100);
    ASSERT_TRUE(after_restart_100.has_value());
    EXPECT_EQ(std::get<int64_t>(after_restart_100->values[1]), 700);

    auto after_restart_200 = engine->lookup_visible("mvcc4", columns, 7, 200);
    ASSERT_TRUE(after_restart_200.has_value());
    EXPECT_EQ(std::get<int64_t>(after_restart_200->values[1]), 777);
}

// ============================================================================
// Table Schema 测试
// ============================================================================

class TableSchemaTest : public ::testing::Test {};

TEST_F(TableSchemaTest, TableWithColumns) {
    std::vector<Column> columns = { Column{ "employees", "id", TypeKind::Int64, 1, 100 },
                                    Column{ "employees", "name", TypeKind::Text, 2, 100 },
                                    Column{ "employees", "department", TypeKind::Text, 3, 100 },
                                    Column{ "employees", "salary", TypeKind::Int64, 4, 100 } };

    Table table("employees", columns);

    EXPECT_EQ(table.name(), "employees");
    EXPECT_EQ(table.columns().size(), 4u);
    EXPECT_EQ(table.columns()[0].name, "id");
    EXPECT_EQ(table.columns()[3].name, "salary");
}

// ============================================================================
// 索引测试
// ============================================================================

class IndexTest : public ::testing::Test {
protected:
    std::unique_ptr<TempDirectory> temp_dir;
    std::unique_ptr<StorageEngine> engine;

    void SetUp() override {
        // 清理上一个测试可能残留的 WAL 文件句柄
        storage_internal::WalManager::instance().clear_all();
        temp_dir = std::make_unique<TempDirectory>();
        engine = std::make_unique<LSMTreeEngine>(temp_dir->path());
    }

    void TearDown() override {
        engine.reset();
        // 清理 WalManager 中的所有文件句柄，确保 WAL 文件可以被删除
        storage_internal::WalManager::instance().clear_all();
        temp_dir.reset();
    }
};

TEST_F(IndexTest, CreateIndex) {
    std::vector<Column> columns = { Column{ "idx_test", "id", TypeKind::Int64, 0, 0 },
                                    Column{ "idx_test", "name", TypeKind::Text, 0, 0 } };

    engine->create_table("idx_test", columns);

    // 初始状态没有索引
    auto indexes = engine->list_indexes("idx_test");
    EXPECT_TRUE(indexes.empty());

    // 创建索引
    engine->create_index_file("idx_test", "name");

    // 现在应该有索引
    indexes = engine->list_indexes("idx_test");
    EXPECT_EQ(indexes.size(), 1u);
}

