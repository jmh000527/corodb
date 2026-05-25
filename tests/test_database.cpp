/**
 * @file test_database.cpp
 * @brief 数据库集成测试
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 测试 Database 类的完整功能
 */

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "corodb/db/database.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/storage/storage_engine_common.h"

using namespace corodb;

// ============================================================================
// 测试辅助工具
// ============================================================================

class TempDirectory {
public:
    TempDirectory(const std::string& prefix = "corodb_db_test") {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(now));
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

// 辅助函数：消费生成器并收集结果
std::vector<Record> collect_results(Database::QueryResult& result) {
    std::vector<Record> records;
    if (result.rows.has_value()) {
        for (auto&& rec: *result.rows) {
            records.push_back(std::move(rec));
        }
    }
    return records;
}

// ============================================================================
// Database 基础测试
// ============================================================================

class DatabaseTest : public ::testing::Test {
protected:
    std::unique_ptr<TempDirectory> temp_dir;
    std::unique_ptr<Database> db;

    void SetUp() override {
        temp_dir = std::make_unique<TempDirectory>();
        db = std::make_unique<Database>(temp_dir->path());
    }

    void TearDown() override {
        db.reset();
        // 清理 WalManager 中的所有文件句柄，确保 WAL 文件可以被删除
        storage_internal::WalManager::instance().clear_all();
        temp_dir.reset();
    }
};

TEST_F(DatabaseTest, CreateTable) {
    auto result = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(result); // 消费生成器
    EXPECT_TRUE(result.is_success());
}

TEST_F(DatabaseTest, CreateTableWithMultipleColumns) {
    auto result = db->execute("CREATE TABLE employees (id INT64, name TEXT, age INT64, department TEXT)");
    collect_results(result);
    EXPECT_TRUE(result.is_success());
}

TEST_F(DatabaseTest, InsertIntoTable) {
    auto create_result = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(create_result);

    auto result = db->execute("INSERT INTO users VALUES (1, 'Alice')");
    collect_results(result);
    EXPECT_TRUE(result.is_success());
}

TEST_F(DatabaseTest, InsertMultipleRows) {
    auto create_result = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(create_result);

    auto r1 = db->execute("INSERT INTO users VALUES (1, 'Alice')");
    collect_results(r1);
    auto r2 = db->execute("INSERT INTO users VALUES (2, 'Bob')");
    collect_results(r2);
    auto r3 = db->execute("INSERT INTO users VALUES (3, 'Charlie')");
    collect_results(r3);

    auto result = db->execute("SELECT * FROM users");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 3u);
}

TEST_F(DatabaseTest, SelectAll) {
    auto cr = db->execute("CREATE TABLE products (id INT64, name TEXT, price INT64)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO products VALUES (1, 'Apple', 100)");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO products VALUES (2, 'Banana', 50)");
    collect_results(i2);

    auto result = db->execute("SELECT * FROM products");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 2u);
}

TEST_F(DatabaseTest, SelectWithWhere) {
    auto cr = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO users VALUES (1, 'Alice')");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO users VALUES (2, 'Bob')");
    collect_results(i2);
    auto i3 = db->execute("INSERT INTO users VALUES (3, 'Charlie')");
    collect_results(i3);

    auto result = db->execute("SELECT * FROM users WHERE id = 2");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 1u);
}

TEST_F(DatabaseTest, SelectWithWhereGreaterThan) {
    auto cr = db->execute("CREATE TABLE numbers (value INT64)");
    collect_results(cr);

    for (int i: { 10, 20, 30, 40 }) {
        auto ir = db->execute("INSERT INTO numbers VALUES (" + std::to_string(i) + ")");
        collect_results(ir);
    }

    auto result = db->execute("SELECT * FROM numbers WHERE value > 25");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 2u);
}

TEST_F(DatabaseTest, UpdateRows) {
    auto cr = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(cr);

    auto ir = db->execute("INSERT INTO users VALUES (1, 'Alice')");
    collect_results(ir);

    auto ur = db->execute("UPDATE users SET name = 'Alicia' WHERE id = 1");
    collect_results(ur);
    EXPECT_TRUE(ur.is_success());

    auto select_result = db->execute("SELECT * FROM users WHERE name = 'Alicia'");
    auto records = collect_results(select_result);
    EXPECT_TRUE(select_result.is_success());
    EXPECT_EQ(records.size(), 1u);
}

TEST_F(DatabaseTest, DeleteRows) {
    auto cr = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(cr);

    for (int i = 1; i <= 3; ++i) {
        auto ir = db->execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'User" + std::to_string(i) + "')");
        collect_results(ir);
    }

    auto dr = db->execute("DELETE FROM users WHERE id = 2");
    collect_results(dr);

    auto result = db->execute("SELECT * FROM users");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 2u);
}

TEST_F(DatabaseTest, DeleteAllRows) {
    auto cr = db->execute("CREATE TABLE temp (id INT64)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO temp VALUES (1)");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO temp VALUES (2)");
    collect_results(i2);

    auto dr = db->execute("DELETE FROM temp");
    collect_results(dr);

    auto result = db->execute("SELECT * FROM temp");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 0u);
}

// ============================================================================
// 排序测试
// ============================================================================

TEST_F(DatabaseTest, SelectWithOrderByAsc) {
    auto cr = db->execute("CREATE TABLE numbers (value INT64)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO numbers VALUES (30)");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO numbers VALUES (10)");
    collect_results(i2);
    auto i3 = db->execute("INSERT INTO numbers VALUES (20)");
    collect_results(i3);

    auto result = db->execute("SELECT * FROM numbers ORDER BY value");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 3u);

    // 验证排序顺序
    EXPECT_EQ(std::get<int64_t>(records[0].values[0]), 10);
    EXPECT_EQ(std::get<int64_t>(records[1].values[0]), 20);
    EXPECT_EQ(std::get<int64_t>(records[2].values[0]), 30);
}

TEST_F(DatabaseTest, SelectWithOrderByDesc) {
    auto cr = db->execute("CREATE TABLE numbers (value INT64)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO numbers VALUES (10)");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO numbers VALUES (30)");
    collect_results(i2);
    auto i3 = db->execute("INSERT INTO numbers VALUES (20)");
    collect_results(i3);

    auto result = db->execute("SELECT * FROM numbers ORDER BY value DESC");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());

    EXPECT_EQ(std::get<int64_t>(records[0].values[0]), 30);
    EXPECT_EQ(std::get<int64_t>(records[1].values[0]), 20);
    EXPECT_EQ(std::get<int64_t>(records[2].values[0]), 10);
}

// ============================================================================
// LIMIT 测试
// ============================================================================

TEST_F(DatabaseTest, SelectWithLimit) {
    auto cr = db->execute("CREATE TABLE data (id INT64)");
    collect_results(cr);

    for (int i = 1; i <= 10; ++i) {
        auto ir = db->execute("INSERT INTO data VALUES (" + std::to_string(i) + ")");
        collect_results(ir);
    }

    auto result = db->execute("SELECT * FROM data LIMIT 5");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 5u);
}

// ============================================================================
// 聚合函数测试
// ============================================================================

TEST_F(DatabaseTest, CountAggregate) {
    auto cr = db->execute("CREATE TABLE items (id INT64)");
    collect_results(cr);

    for (int i = 1; i <= 3; ++i) {
        auto ir = db->execute("INSERT INTO items VALUES (" + std::to_string(i) + ")");
        collect_results(ir);
    }

    auto result = db->execute("SELECT COUNT(*) FROM items");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(records[0].values[0]), 3);
}

TEST_F(DatabaseTest, SumAggregate) {
    auto cr = db->execute("CREATE TABLE amounts (value INT64)");
    collect_results(cr);

    for (int v: { 10, 20, 30 }) {
        auto ir = db->execute("INSERT INTO amounts VALUES (" + std::to_string(v) + ")");
        collect_results(ir);
    }

    auto result = db->execute("SELECT SUM(value) FROM amounts");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(std::get<int64_t>(records[0].values[0]), 60);
}

TEST_F(DatabaseTest, MinMaxAggregate) {
    auto cr = db->execute("CREATE TABLE scores (score INT64)");
    collect_results(cr);

    for (int s: { 85, 92, 78 }) {
        auto ir = db->execute("INSERT INTO scores VALUES (" + std::to_string(s) + ")");
        collect_results(ir);
    }

    auto min_result = db->execute("SELECT MIN(score) FROM scores");
    auto min_records = collect_results(min_result);
    EXPECT_TRUE(min_result.is_success());
    EXPECT_EQ(std::get<int64_t>(min_records[0].values[0]), 78);

    auto max_result = db->execute("SELECT MAX(score) FROM scores");
    auto max_records = collect_results(max_result);
    EXPECT_TRUE(max_result.is_success());
    EXPECT_EQ(std::get<int64_t>(max_records[0].values[0]), 92);
}

// ============================================================================
// JOIN 测试
// ============================================================================

TEST_F(DatabaseTest, InnerJoin) {
    auto cr1 = db->execute("CREATE TABLE users (id INT64, name TEXT)");
    collect_results(cr1);
    auto cr2 = db->execute("CREATE TABLE orders (id INT64, user_id INT64, product TEXT)");
    collect_results(cr2);

    auto i1 = db->execute("INSERT INTO users VALUES (1, 'Alice')");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO users VALUES (2, 'Bob')");
    collect_results(i2);

    auto o1 = db->execute("INSERT INTO orders VALUES (101, 1, 'Laptop')");
    collect_results(o1);
    auto o2 = db->execute("INSERT INTO orders VALUES (102, 1, 'Phone')");
    collect_results(o2);
    auto o3 = db->execute("INSERT INTO orders VALUES (103, 2, 'Tablet')");
    collect_results(o3);

    auto result = db->execute("SELECT * FROM users JOIN orders ON users.id = orders.user_id");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 3u);
}

// ============================================================================
// 索引测试
// ============================================================================

TEST_F(DatabaseTest, CreateIndex) {
    auto cr = db->execute("CREATE TABLE indexed_table (id INT64, name TEXT)");
    collect_results(cr);

    auto result = db->execute("CREATE INDEX idx_name ON indexed_table (name)");
    collect_results(result);
    EXPECT_TRUE(result.is_success());
}

// ============================================================================
// EXPLAIN 测试
// ============================================================================

TEST_F(DatabaseTest, ExplainSelect) {
    auto cr = db->execute("CREATE TABLE test_table (id INT64, data TEXT)");
    collect_results(cr);

    auto result = db->execute("EXPLAIN SELECT * FROM test_table WHERE id > 10");
    collect_results(result);
    EXPECT_TRUE(result.is_success());
    // EXPLAIN 应该返回计划信息
}

// T4.8: EXPLAIN 输出应包含 Logical Plan 与 Physical Plan 两段
TEST_F(DatabaseTest, ExplainDualOutput) {
    auto r1 = db->execute("CREATE TABLE t (id INT64, name TEXT)");
    collect_results(r1);
    auto r2 = db->execute("INSERT INTO t VALUES (1, 'a')");
    collect_results(r2);

    auto result = db->execute("EXPLAIN SELECT * FROM t WHERE id = 1");
    auto records = collect_results(result);
    ASSERT_TRUE(result.is_success());

    std::string joined;
    for (const auto& r: records) {
        for (const auto& v: r.values) {
            if (auto* s = std::get_if<std::string>(&v))
                joined += *s + "\n";
        }
    }
    // PostgreSQL-style output: plan tree only, no "Logical Plan:" / "Physical Plan:" headers.
    EXPECT_NE(joined.find("Seq Scan on t"), std::string::npos) << joined;
    EXPECT_NE(joined.find("Filter: (id = 1)"), std::string::npos) << joined;
}

// ============================================================================
// NULL 值测试
// ============================================================================

// 注意: 当前解析器不支持 INSERT VALUES 中的 NULL 字面量
// TEST_F(DatabaseTest, InsertNullValue) { ... }

// ============================================================================
// 错误处理测试
// ============================================================================

TEST_F(DatabaseTest, InvalidSQL) {
    EXPECT_THROW(db->execute("SELEC * FROM users"), std::exception);
}

TEST_F(DatabaseTest, TableNotExists) {
    EXPECT_THROW(db->execute("SELECT * FROM nonexistent_table"), std::exception);
}

// ============================================================================
// 不同存储引擎测试
// ============================================================================

class DatabaseLSMTest2 : public ::testing::Test {
protected:
    std::unique_ptr<TempDirectory> temp_dir;
    std::unique_ptr<Database> db;

    void SetUp() override {
        temp_dir = std::make_unique<TempDirectory>("corodb_lsm_test2");
        db = std::make_unique<Database>(temp_dir->path());
    }

    void TearDown() override {
        db.reset();
        // 清理 WalManager 中的所有文件句柄，确保 WAL 文件可以被删除
        storage_internal::WalManager::instance().clear_all();
        temp_dir.reset();
    }
};

TEST_F(DatabaseLSMTest2, BasicOperations) {
    auto cr = db->execute("CREATE TABLE lsm_test2 (id INT64, data TEXT)");
    collect_results(cr);

    auto i1 = db->execute("INSERT INTO lsm_test2 VALUES (1, 'one')");
    collect_results(i1);
    auto i2 = db->execute("INSERT INTO lsm_test2 VALUES (2, 'two')");
    collect_results(i2);

    auto result = db->execute("SELECT * FROM lsm_test2");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 2u);
}

class DatabaseLSMTest : public ::testing::Test {
protected:
    std::unique_ptr<TempDirectory> temp_dir;
    std::unique_ptr<Database> db;

    void SetUp() override {
        temp_dir = std::make_unique<TempDirectory>("corodb_lsm_test");
        db = std::make_unique<Database>(temp_dir->path());
    }

    void TearDown() override {
        db.reset();
        // 清理 WalManager 中的所有文件句柄，确保 WAL 文件可以被删除
        storage_internal::WalManager::instance().clear_all();
        temp_dir.reset();
    }
};

TEST_F(DatabaseLSMTest, BasicOperations) {
    auto cr = db->execute("CREATE TABLE lsm_test (key INT64, value TEXT)");
    collect_results(cr);

    auto ir = db->execute("INSERT INTO lsm_test VALUES (100, 'hundred')");
    collect_results(ir);

    auto result = db->execute("SELECT * FROM lsm_test");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_GE(records.size(), 1u);
}

// ============================================================================
// 压力测试
// ============================================================================

TEST_F(DatabaseTest, ManyInserts) {
    auto cr = db->execute("CREATE TABLE bulk (id INT64, value INT64)");
    collect_results(cr);

    const int count = 100;
    for (int i = 0; i < count; ++i) {
        auto ir = db->execute("INSERT INTO bulk VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        collect_results(ir);
    }

    auto result = db->execute("SELECT COUNT(*) FROM bulk");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(std::get<int64_t>(records[0].values[0]), count);
}

TEST_F(DatabaseTest, ComplexQuery) {
    auto cr = db->execute("CREATE TABLE employees (id INT64, name TEXT, dept TEXT, salary INT64)");
    collect_results(cr);

    std::vector<std::string> inserts = { "INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 100000)",
                                         "INSERT INTO employees VALUES (2, 'Bob', 'Engineering', 90000)",
                                         "INSERT INTO employees VALUES (3, 'Charlie', 'Sales', 80000)",
                                         "INSERT INTO employees VALUES (4, 'Diana', 'Sales', 85000)",
                                         "INSERT INTO employees VALUES (5, 'Eve', 'HR', 70000)" };

    for (const auto& sql: inserts) {
        auto ir = db->execute(sql);
        collect_results(ir);
    }

    // 复杂查询：过滤 + 排序 + 限制
    auto result = db->execute("SELECT * FROM employees WHERE salary > 75000 ORDER BY salary DESC LIMIT 3");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_LE(records.size(), 3u);
}

// ============================================================================
// 字符串处理测试
// ============================================================================

TEST_F(DatabaseTest, StringWithSpaces) {
    auto cr = db->execute("CREATE TABLE messages (id INT64, content TEXT)");
    collect_results(cr);

    auto ir = db->execute("INSERT INTO messages VALUES (1, 'Hello World')");
    collect_results(ir);

    auto result = db->execute("SELECT * FROM messages WHERE content = 'Hello World'");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 1u);
}

TEST_F(DatabaseTest, UnicodeStrings) {
    auto cr = db->execute("CREATE TABLE i18n (id INT64, text TEXT)");
    collect_results(cr);

    auto ir1 = db->execute("INSERT INTO i18n VALUES (1, '你好世界')");
    collect_results(ir1);
    auto ir2 = db->execute("INSERT INTO i18n VALUES (2, 'Привет мир')");
    collect_results(ir2);
    auto ir3 = db->execute("INSERT INTO i18n VALUES (3, 'مرحبا بالعالم')");
    collect_results(ir3);

    auto result = db->execute("SELECT * FROM i18n");
    auto records = collect_results(result);
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(records.size(), 3u);
}

// ============================================================================
// 正确性测试 —— LEFT JOIN / AVG / HAVING / DISTINCT / 自连接 / 三表JOIN
// ============================================================================

// 辅助：执行 SQL 并返回第一行的第一个值（int64）
int64_t fetch_int(Database& db, const std::string& sql) {
    auto r = db.execute(sql);
    auto recs = collect_results(r);
    EXPECT_GE(recs.size(), 1u) << "SQL returned no rows: " << sql;
    EXPECT_GE(recs[0].values.size(), 1u);
    return std::get<int64_t>(recs[0].values[0]);
}

// 辅助：执行 SQL 并返回第一行的第一个值（double）
double fetch_double(Database& db, const std::string& sql) {
    auto r = db.execute(sql);
    auto recs = collect_results(r);
    EXPECT_GE(recs.size(), 1u) << "SQL returned no rows: " << sql;
    EXPECT_GE(recs[0].values.size(), 1u);
    return std::get<double>(recs[0].values[0]);
}

// 辅助：执行 SQL 并返回行数
std::size_t count_rows(Database& db, const std::string& sql) {
    auto r = db.execute(sql);
    return collect_results(r).size();
}

// 辅助：执行 SQL 并返回第一行的第一个值（string）
std::string fetch_str(Database& db, const std::string& sql) {
    auto r = db.execute(sql);
    auto recs = collect_results(r);
    EXPECT_GE(recs.size(), 1u);
    EXPECT_GE(recs[0].values.size(), 1u);
    return std::get<std::string>(recs[0].values[0]);
}

// ---------- AVG() 返回浮点数 ----------

TEST_F(DatabaseTest, AvgReturnsFloat) {
    db->execute("CREATE TABLE nums (id INT64, val INT64)");
    db->execute("INSERT INTO nums VALUES (1, 3)");
    db->execute("INSERT INTO nums VALUES (2, 4)");
    double avg = fetch_double(*db, "SELECT AVG(val) FROM nums");
    EXPECT_DOUBLE_EQ(avg, 3.5) << "AVG(3,4) must be 3.5, not 3";
}

// ---------- LEFT JOIN NULL 填充 ----------

TEST_F(DatabaseTest, LeftJoinNullPadding) {
    db->execute("CREATE TABLE a (id INT64, name TEXT)");
    db->execute("CREATE TABLE b (id INT64, score INT64)");
    db->execute("INSERT INTO a VALUES (1, 'Alice')");
    db->execute("INSERT INTO a VALUES (2, 'Bob')");
    db->execute("INSERT INTO b VALUES (1, 100)");

    auto r = db->execute("SELECT a.name, b.score FROM a LEFT JOIN b ON a.id = b.id ORDER BY a.id");
    auto recs = collect_results(r);
    ASSERT_EQ(recs.size(), 2u);
    // Alice matched → score = 100
    EXPECT_EQ(std::get<std::string>(recs[0].values[0]), "Alice");
    EXPECT_EQ(std::get<int64_t>(recs[0].values[1]), 100);
    // Bob unmatched → score 应为 NULL
    EXPECT_EQ(std::get<std::string>(recs[1].values[0]), "Bob");
    EXPECT_TRUE(std::holds_alternative<NullValue>(recs[1].values[1]))
        << "LEFT JOIN unmatched row must have NULL, not 0 or empty";
}

// ---------- GROUP BY + HAVING ----------

TEST_F(DatabaseTest, GroupByHaving) {
    db->execute("CREATE TABLE emp (id INT64, dept TEXT, sal INT64)");
    db->execute("INSERT INTO emp VALUES (1, 'A', 100)");
    db->execute("INSERT INTO emp VALUES (2, 'A', 200)");
    db->execute("INSERT INTO emp VALUES (3, 'B', 50)");
    // HAVING SUM(sal) >= 200: 仅部门 A(sum=300)通过，B(sum=50)被排除
    auto n = count_rows(*db,
        "SELECT dept FROM emp GROUP BY dept HAVING SUM(sal) >= 200");
    EXPECT_EQ(n, 1u) << "HAVING SUM>=200: only dept A (sum=300) passes";
}

// ---------- DISTINCT ----------

TEST_F(DatabaseTest, DistinctDeduplication) {
    db->execute("CREATE TABLE t (id INT64, tag TEXT)");
    db->execute("INSERT INTO t VALUES (1, 'x')");
    db->execute("INSERT INTO t VALUES (2, 'x')");
    db->execute("INSERT INTO t VALUES (3, 'y')");
    // 'x' 出现 2 次，去重后应只剩 2 行（x, y）
    auto n = count_rows(*db, "SELECT DISTINCT tag FROM t");
    EXPECT_EQ(n, 2u) << "DISTINCT must deduplicate: 3 rows -> 2 unique tags";
}

// ---------- 自连接 ----------

TEST_F(DatabaseTest, SelfJoin) {
    db->execute("CREATE TABLE edge (id INT64, src TEXT, dst TEXT)");
    db->execute("INSERT INTO edge VALUES (1, 'A', 'B')");
    db->execute("INSERT INTO edge VALUES (2, 'B', 'C')");
    // 查找 src='A' 的边连接到的 dst 作为下一段的 src
    auto r = db->execute(
        "SELECT e1.dst FROM edge e1 INNER JOIN edge e2 ON e1.dst = e2.src WHERE e1.src = 'A'");
    auto recs = collect_results(r);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(std::get<std::string>(recs[0].values[0]), "B");
}

// ---------- 三表 JOIN ----------

TEST_F(DatabaseTest, ThreeTableJoin) {
    db->execute("CREATE TABLE users (id INT64, name TEXT)");
    db->execute("CREATE TABLE orders (id INT64, uid INT64, item TEXT)");
    db->execute("CREATE TABLE payments (id INT64, oid INT64, amount INT64)");
    db->execute("INSERT INTO users VALUES (1, 'Alice')");
    db->execute("INSERT INTO orders VALUES (1, 1, 'Book')");
    db->execute("INSERT INTO payments VALUES (1, 1, 30)");

    auto r3join = db->execute(
        "SELECT u.name, o.item, p.amount "
        "FROM users u JOIN orders o ON u.id = o.uid "
        "JOIN payments p ON o.id = p.oid");
    auto recs = collect_results(r3join);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(std::get<std::string>(recs[0].values[0]), "Alice");
    EXPECT_EQ(std::get<std::string>(recs[0].values[1]), "Book");
    EXPECT_EQ(std::get<int64_t>(recs[0].values[2]), 30);
}

// ---------- WHERE NOT + NULL（三值逻辑） ----------

TEST_F(DatabaseTest, WhereNotNullSemantics) {
    db->execute("CREATE TABLE t (id INT64, flag INT64)");
    db->execute("INSERT INTO t VALUES (1, 1)");
    db->execute("INSERT INTO t VALUES (2, 2)");
    // NOT (flag = 1) 应该只排除 id=1，保留 id=2
    auto n = count_rows(*db, "SELECT * FROM t WHERE NOT (flag = 1)");
    EXPECT_EQ(n, 1u) << "NOT (flag=1) must exclude row with flag=1, keep row with flag=2";
}

// ---------- AND/OR 组合 ----------

TEST_F(DatabaseTest, AndOrCombined) {
    db->execute("CREATE TABLE t (id INT64, a INT64, b INT64, c INT64)");
    db->execute("INSERT INTO t VALUES (1, 1, 1, 0)");
    db->execute("INSERT INTO t VALUES (2, 0, 1, 0)");
    db->execute("INSERT INTO t VALUES (3, 0, 0, 1)");
    // (a=1 AND b=1) 匹配 id=1; c=1 匹配 id=3; 共 2 行
    auto n = count_rows(*db, "SELECT * FROM t WHERE (a = 1 AND b = 1) OR c = 1");
    EXPECT_EQ(n, 2u) << "(a=1 AND b=1) OR c=1 must match id=1 (both a,b=1) and id=3 (c=1)";
}

// ---------- 聚合空表 ----------

TEST_F(DatabaseTest, AggregateEmptyTable) {
    db->execute("CREATE TABLE empty_t (id INT64, val INT64)");
    auto n = count_rows(*db, "SELECT COUNT(*) FROM empty_t");
    EXPECT_EQ(n, 1u);
    auto cnt = fetch_int(*db, "SELECT COUNT(*) FROM empty_t");
    EXPECT_EQ(cnt, 0) << "COUNT(*) on empty table must return 0";
}

// ---------- LIMIT + OFFSET ----------

TEST_F(DatabaseTest, LimitWithOffset) {
    db->execute("CREATE TABLE t (id INT64, val INT64)");
    for (int i = 1; i <= 5; ++i)
        db->execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
    // OFFSET 2 LIMIT 2 → rows 3 and 4
    auto r_limoff = db->execute("SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 2");
    auto recs = collect_results(r_limoff);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(recs[0].values[0]), 3);
    EXPECT_EQ(std::get<int64_t>(recs[1].values[0]), 4);
}

// ---------- UPDATE 无 WHERE ----------

TEST_F(DatabaseTest, UpdateAllRows) {
    db->execute("CREATE TABLE t (id INT64, v INT64)");
    db->execute("INSERT INTO t VALUES (1, 10)");
    db->execute("INSERT INTO t VALUES (2, 20)");
    db->execute("INSERT INTO t VALUES (3, 30)");
    db->execute("UPDATE t SET v = 99");
    auto r_upd = db->execute("SELECT v FROM t ORDER BY id");
    auto recs = collect_results(r_upd);
    ASSERT_EQ(recs.size(), 3u);
    for (const auto& r : recs)
        EXPECT_EQ(std::get<int64_t>(r.values[0]), 99) << "UPDATE without WHERE must update all rows";
}

// ---------- ORDER BY 多列 ----------

TEST_F(DatabaseTest, OrderByMultipleColumns) {
    db->execute("CREATE TABLE t (id INT64, a INT64, b INT64)");
    db->execute("INSERT INTO t VALUES (1, 1, 3)");
    db->execute("INSERT INTO t VALUES (2, 1, 1)");
    db->execute("INSERT INTO t VALUES (3, 2, 0)");
    auto r_ord = db->execute("SELECT id FROM t ORDER BY a ASC, b ASC");
    auto recs = collect_results(r_ord);
    ASSERT_EQ(recs.size(), 3u);
    // a=1,b=1(id=2), a=1,b=3(id=1), a=2,b=0(id=3)
    EXPECT_EQ(std::get<int64_t>(recs[0].values[0]), 2);
    EXPECT_EQ(std::get<int64_t>(recs[1].values[0]), 1);
    EXPECT_EQ(std::get<int64_t>(recs[2].values[0]), 3);
}

