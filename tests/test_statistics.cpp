/**
 * @file test_statistics.cpp
 * @brief 统计信息基础设施测试（OPTIMIZER_UPGRADE_PLAN Phase 1，T1.9）
 *
 * 覆盖：
 *   - T1.1 ColumnStats/TableStats 序列化 round-trip
 *   - T1.2 采集器：全量扫描 / 采样 / MCV / 直方图 / NDV / null_frac
 *   - T1.5 选择率估计：等值 / 范围 / IN / IS NULL / AND / OR
 *   - T1.3 ANALYZE 命令（经 Database 集成）
 *   - T1.4 持久化：重启加载 / DROP TABLE 清理
 *   - T1.7 EXPLAIN (rows=N) 注解
 *   - T1.8 auto-ANALYZE 触发与写计数器
 */

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "corodb/common/config.h"
#include "corodb/db/database.h"
#include "corodb/optimizer/stats/selectivity.h"
#include "corodb/storage/statistics.h"
#include "corodb/storage/statistics_collector.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/storage/table.h"

using namespace corodb;

// ============================================================================
// 工具
// ============================================================================

namespace {

    class TempDirectory {
    public:
        explicit TempDirectory(const std::string& prefix) {
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(now) + "_" +
                                                              std::to_string(++counter));
            std::filesystem::create_directories(path_);
        }
        ~TempDirectory() {
            try {
                std::filesystem::remove_all(path_);
            } catch (...) {
            }
        }
        std::string path() const {
            return path_.string();
        }

    private:
        static inline int counter = 0;
        std::filesystem::path path_;
    };

    /** @brief 内存表（无存储引擎），用于采集器/选择率单元测试。 */
    std::shared_ptr<Table> make_memory_table(const std::string& name) {
        std::vector<Column> cols;
        cols.push_back(Column{ name, "id", TypeKind::Int64 });
        cols.push_back(Column{ name, "val", TypeKind::Int64 });
        return std::make_shared<Table>(name, std::move(cols), static_cast<StorageEngine*>(nullptr));
    }

    Row row_of(int64_t id, int64_t val) {
        return Row{ std::vector<Value>{ id, val } };
    }

    /** @brief 消费 SELECT/EXPLAIN 结果为拼接文本。 */
    std::string rows_to_text(Database::QueryResult& r) {
        std::string out;
        if (r.rows.has_value()) {
            for (auto&& rec : *r.rows) {
                for (const auto& v : rec.values) {
                    if (std::holds_alternative<std::string>(v))
                        out += std::get<std::string>(v) + "\n";
                    else if (std::holds_alternative<int64_t>(v))
                        out += std::to_string(std::get<int64_t>(v)) + "\n";
                }
            }
        }
        return out;
    }

} // namespace

// ============================================================================
// T1.1 序列化
// ============================================================================

TEST(StatisticsSerialization, ColumnStatsRoundTripLossless) {
    ColumnStats s;
    s.column_name = "val";
    s.type = TypeKind::Int64;
    s.total_rows = 1000;
    s.null_count = 25;
    s.null_frac = 0.025;
    s.ndistinct = 40;
    s.mcv_values = { Value{ int64_t(1) }, Value{ int64_t(2) } };
    s.mcv_freqs = { 0.5, 0.2 };
    s.histogram_bounds = { Value{ int64_t(1) }, Value{ int64_t(10) }, Value{ int64_t(50) },
                           Value{ int64_t(100) } };
    s.correlation = -0.3;
    s.stats_ts = 123456;

    auto d = ColumnStats::deserialize(s.serialize());
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->column_name, "val");
    EXPECT_EQ(d->type, TypeKind::Int64);
    EXPECT_EQ(d->total_rows, 1000u);
    EXPECT_EQ(d->null_count, 25u);
    EXPECT_DOUBLE_EQ(d->null_frac, 0.025);
    EXPECT_EQ(d->ndistinct, 40u);
    ASSERT_EQ(d->mcv_values.size(), 2u);
    ASSERT_EQ(d->mcv_freqs.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(d->mcv_values[0]), 1);
    EXPECT_DOUBLE_EQ(d->mcv_freqs[0], 0.5);
    ASSERT_EQ(d->histogram_bounds.size(), 4u);
    EXPECT_EQ(std::get<int64_t>(d->histogram_bounds[3]), 100);
    EXPECT_DOUBLE_EQ(d->correlation, -0.3);
    EXPECT_EQ(d->stats_ts, 123456u);
}

TEST(StatisticsSerialization, ColumnStatsDeserializeGarbageFails) {
    EXPECT_FALSE(ColumnStats::deserialize("").has_value());
    EXPECT_FALSE(ColumnStats::deserialize("garbage").has_value());
    std::string truncated = ColumnStats{}.serialize();
    if (truncated.size() > 2)
        EXPECT_FALSE(ColumnStats::deserialize(truncated.substr(0, truncated.size() - 2)).has_value());
}

TEST(StatisticsSerialization, TableStatsRoundTripMultipleColumns) {
    TableStats ts;
    ts.table_name = "emp";
    ts.total_rows = 500;
    ts.stats_ts = 999;

    ColumnStats a;
    a.column_name = "id";
    a.type = TypeKind::Int64;
    a.total_rows = 500;
    a.ndistinct = 500;
    ColumnStats b;
    b.column_name = "name";
    b.type = TypeKind::Text;
    b.total_rows = 500;
    b.ndistinct = 100;
    b.mcv_values = { Value{ std::string("bob") } };
    b.mcv_freqs = { 0.4 };
    ts.columns["id"] = a;
    ts.columns["name"] = b;

    auto d = TableStats::deserialize(ts.serialize());
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->table_name, "emp");
    EXPECT_EQ(d->total_rows, 500u);
    EXPECT_EQ(d->columns.size(), 2u);
    ASSERT_NE(d->column("name"), nullptr);
    EXPECT_EQ(d->column("name")->ndistinct, 100u);
    ASSERT_EQ(d->column("name")->mcv_values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(d->column("name")->mcv_values[0]), "bob");
}

// ============================================================================
// T1.2 采集器
// ============================================================================

TEST(StatisticsCollectorTest, FullScanExactStats) {
    auto t = make_memory_table("t1");
    for (int64_t i = 1; i <= 100; ++i)
        t->insert(row_of(i, i % 10)); // val: 10 个去重值，均匀

    StatisticsCollector collector;
    auto stats = collector.collect(*t, 7);

    EXPECT_EQ(stats.total_rows, 100u);
    ASSERT_NE(stats.column("val"), nullptr);
    EXPECT_EQ(stats.column("val")->ndistinct, 10u);
    EXPECT_EQ(stats.column("val")->null_count, 0u);
    EXPECT_NEAR(stats.column("val")->null_frac, 0.0, 1e-9);
    // 全量扫描下 id 列 NDV = 100。
    EXPECT_EQ(stats.column("id")->ndistinct, 100u);
    EXPECT_EQ(stats.stats_ts, 7u);
}

TEST(StatisticsCollectorTest, SkewCapturedByMcv) {
    auto t = make_memory_table("t2");
    // 90% 值=1，10% 互异随机值。总行数 800 ≤ sample_target×3（900）→ 全量扫描，
    // 频率不因采样缩放，freq ≈ 0.9（覆盖 PHASE1_TASK_LIST 倾斜验收项）。
    for (int64_t i = 0; i < 720; ++i)
        t->insert(row_of(i, 1));
    for (int64_t i = 720; i < 800; ++i)
        t->insert(row_of(i, 100000 + i));

    StatisticsCollector collector;
    auto stats = collector.collect(*t, 1);

    const ColumnStats* cs = stats.column("val");
    ASSERT_NE(cs, nullptr);
    ASSERT_GE(cs->mcv_values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(cs->mcv_values[0]), 1);
    EXPECT_NEAR(cs->mcv_freqs[0], 0.9, 0.02);
    // MCV=1 不应再出现在直方图边界里（排除 MCV 后分桶）。
    for (const auto& b : cs->histogram_bounds)
        EXPECT_FALSE(std::holds_alternative<int64_t>(b) && std::get<int64_t>(b) == 1);
}

TEST(StatisticsCollectorTest, HistogramBoundsSortedMinMax) {
    auto t = make_memory_table("t3");
    for (int64_t i = 1; i <= 500; ++i)
        t->insert(row_of(i, i * 3));

    StatisticsCollector collector;
    auto stats = collector.collect(*t, 1);
    const ColumnStats* cs = stats.column("val");
    ASSERT_NE(cs, nullptr);
    ASSERT_GE(cs->histogram_bounds.size(), 2u);
    ValueLess lt;
    for (std::size_t i = 1; i < cs->histogram_bounds.size(); ++i)
        EXPECT_FALSE(lt(cs->histogram_bounds[i], cs->histogram_bounds[i - 1])) << "bounds[" << i << "]";
    // 无 MCV 时直方图覆盖全值域。
    EXPECT_EQ(std::get<int64_t>(cs->histogram_bounds.front()), 3);
    EXPECT_EQ(std::get<int64_t>(cs->histogram_bounds.back()), 1500);
}

TEST(StatisticsCollectorTest, NullFractionAccurate) {
    auto t = make_memory_table("t4");
    for (int64_t i = 0; i < 200; ++i) {
        if (i % 4 == 0)
            t->insert(Row{ std::vector<Value>{ i, NullValue{} } });
        else
            t->insert(row_of(i, i));
    }
    StatisticsCollector collector;
    auto stats = collector.collect(*t, 1);
    const ColumnStats* cs = stats.column("val");
    ASSERT_NE(cs, nullptr);
    EXPECT_NEAR(cs->null_frac, 0.25, 1e-9);
    EXPECT_EQ(cs->null_count, 50u);
}

TEST(StatisticsCollectorTest, UniformNdvErrorUnder15Percent) {
    // 均匀 1000 值列（PHASE1_TASK_LIST 验收：NDV 误差 < 15%）。
    auto t = make_memory_table("t5");
    for (int64_t i = 0; i < 10000; ++i)
        t->insert(row_of(i, i % 1000));

    StatisticsCollector collector; // 默认 sample_target=300 → 采样模式
    auto stats = collector.collect(*t, 1);
    const ColumnStats* cs = stats.column("val");
    ASSERT_NE(cs, nullptr);
    const double err = std::abs(static_cast<double>(cs->ndistinct) - 1000.0) / 1000.0;
    EXPECT_LT(err, 0.15) << "ndistinct=" << cs->ndistinct;
}

// ============================================================================
// T1.5 选择率估计
// ============================================================================

namespace {

    /** @brief 构造手工列统计：1000 行，均匀 1000 值（无 MCV），32 桶等高直方图。 */
    ColumnStats uniform_stats() {
        ColumnStats s;
        s.column_name = "c";
        s.type = TypeKind::Int64;
        s.total_rows = 1000;
        s.null_count = 0;
        s.null_frac = 0.0;
        s.ndistinct = 1000;
        for (int b = 0; b <= 32; ++b)
            s.histogram_bounds.push_back(Value{ static_cast<int64_t>(b * 100) });
        return s;
    }

    /** @brief 构造带 MCV 的统计：值 1 占 50%，其余均匀 500 值。 */
    ColumnStats mcv_stats() {
        ColumnStats s;
        s.column_name = "c";
        s.type = TypeKind::Int64;
        s.total_rows = 1000;
        s.ndistinct = 501;
        s.mcv_values.push_back(Value{ int64_t(1) });
        s.mcv_freqs.push_back(0.5);
        // 直方图覆盖其余 0.5 人口，值域 [100, 600]。
        for (int b = 0; b <= 16; ++b)
            s.histogram_bounds.push_back(Value{ static_cast<int64_t>(100 + b * 31) });
        return s;
    }

} // namespace

TEST(SelectivityTest, EqUniformEqualsInverseNdv) {
    auto s = uniform_stats();
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_eq(&s, Value{ int64_t(42) }), 0.001, 1e-9);
}

TEST(SelectivityTest, EqMcvHitReturnsExactFreq) {
    auto s = mcv_stats();
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_eq(&s, Value{ int64_t(1) }), 0.5, 1e-9);
}

TEST(SelectivityTest, EqMcvMissSpreadsRemainder) {
    auto s = mcv_stats();
    // 未命中：其余 0.5 行均摊到 500 个非 MCV 值。
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_eq(&s, Value{ int64_t(350) }), 0.001, 1e-9);
}

TEST(SelectivityTest, EqNoStatsFallsBackToDefault) {
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_eq(nullptr, Value{ int64_t(1) }),
                opt::SelectivityEstimator::kDefaultEqSelectivity, 1e-9);
    ColumnStats empty;
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_eq(&empty, Value{ int64_t(1) }),
                opt::SelectivityEstimator::kDefaultEqSelectivity, 1e-9);
}

TEST(SelectivityTest, RangeMidpointAboutHalf) {
    auto s = uniform_stats();
    // 值域 [0, 3200]：> 1600 → 约一半人口。
    double hi_half = opt::SelectivityEstimator::selectivity_range(
            &s, std::nullopt, false, std::optional<Value>(int64_t(1600)), false);
    EXPECT_NEAR(hi_half, 0.5, 0.05);
    // BETWEEN 800 AND 2400 覆盖值域正中一半。
    double between = opt::SelectivityEstimator::selectivity_between(&s, Value{ int64_t(800) },
                                                                    Value{ int64_t(2400) });
    EXPECT_NEAR(between, 0.5, 0.05);
}

TEST(SelectivityTest, RangeNoHistogramFallsBack) {
    ColumnStats s;
    s.total_rows = 100;
    s.ndistinct = 10;
    double sel = opt::SelectivityEstimator::selectivity_range(&s, std::optional<Value>(int64_t(1)), true,
                                                              std::optional<Value>(int64_t(5)), true);
    EXPECT_NEAR(sel, opt::SelectivityEstimator::kDefaultRangeSelectivity, 1e-9);
}

TEST(SelectivityTest, InListSumsDedupCapped) {
    auto s = mcv_stats();
    // IN (1, 1, 350) = eq(1) + eq(350) ≈ 0.5 + 0.001。
    double sel = opt::SelectivityEstimator::selectivity_in_list(
            &s, { Value{ int64_t(1) }, Value{ int64_t(1) }, Value{ int64_t(350) } });
    EXPECT_NEAR(sel, 0.501, 1e-3);
    // 全量超界封顶 1。
    std::vector<Value> many;
    for (int i = 0; i < 2000; ++i)
        many.push_back(Value{ int64_t(i) });
    EXPECT_LE(opt::SelectivityEstimator::selectivity_in_list(&s, many), 1.0);
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_in_list(&s, {}), 0.0, 1e-9);
}

TEST(SelectivityTest, IsNullUsesNullFrac) {
    auto s = uniform_stats();
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_is_null(&s), 0.0, 1e-9);
    s.null_frac = 0.3;
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_is_null(&s), 0.3, 1e-9);
    EXPECT_NEAR(opt::SelectivityEstimator::selectivity_is_null(nullptr), 0.0, 1e-9);
}

TEST(SelectivityTest, CombineAndOr) {
    using opt::SelectivityEstimator;
    EXPECT_NEAR(SelectivityEstimator::combine_and(0.5, 0.4), 0.2, 1e-9);
    EXPECT_NEAR(SelectivityEstimator::combine_or(0.5, 0.4), 0.7, 1e-9);
    EXPECT_NEAR(SelectivityEstimator::combine_or(1.0, 0.4), 1.0, 1e-9);
}

TEST(SelectivityTest, FilterSelectivityOnRealTable) {
    auto t = make_memory_table("t6");
    // val ∈ [1..10] 均匀，100 行。
    for (int64_t i = 1; i <= 100; ++i)
        t->insert(row_of(i, (i % 10) + 1));
    StatisticsCollector collector;
    t->update_stats(collector.collect(*t, 1));

    // 手工构造谓词：val > 5 AND id < 50。
    auto cmp = [](const char* col, CompareOp op, int64_t lit) {
        BoolExpr e;
        e.kind = BoolExpr::Kind::Comparison;
        e.cmp = Comparison{ ColumnRef{ "", col }, op, Literal{ Value{ lit } } };
        return e;
    };
    BoolExpr and_e;
    and_e.kind = BoolExpr::Kind::And;
    and_e.left = std::make_unique<BoolExpr>(cmp("val", CompareOp::Gt, 5));
    and_e.right = std::make_unique<BoolExpr>(cmp("id", CompareOp::Lt, 50));

    double sel = opt::SelectivityEstimator::estimate_filter_selectivity(*t, and_e);
    // 真值 ≈ 0.5 × 0.49 = 0.245；统计估计应接近（误差容忍直方图分桶粒度）。
    EXPECT_GT(sel, 0.1);
    EXPECT_LT(sel, 0.4);

    // OR：val > 5 OR id < 50 ≈ 0.5 + 0.49 - 0.245 ≈ 0.745。
    BoolExpr or_e;
    or_e.kind = BoolExpr::Kind::Or;
    or_e.left = std::make_unique<BoolExpr>(cmp("val", CompareOp::Gt, 5));
    or_e.right = std::make_unique<BoolExpr>(cmp("id", CompareOp::Lt, 50));
    double sel_or = opt::SelectivityEstimator::estimate_filter_selectivity(*t, or_e);
    EXPECT_GT(sel_or, 0.6);
    EXPECT_LT(sel_or, 0.9);

    // 无统计表：回退默认值不崩溃。
    auto t2 = make_memory_table("t7");
    double fallback = opt::SelectivityEstimator::estimate_filter_selectivity(*t2, and_e);
    EXPECT_GT(fallback, 0.0);
    EXPECT_LE(fallback, 1.0);
}

// ============================================================================
// T1.3 / T1.4 / T1.7 / T1.8 — Database 集成
// ============================================================================

class StatisticsDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::make_unique<TempDirectory>("corodb_stats_test");
        saved_data_dir_ = Config::instance().data_dir();
        Config::instance().set_data_dir(temp_dir_->path());
        db_ = std::make_unique<Database>(temp_dir_->path());
    }
    void TearDown() override {
        db_.reset();
        storage_internal::WalManager::instance().clear_all();
        Config::instance().set_data_dir(saved_data_dir_);
        temp_dir_.reset();
    }

    std::unique_ptr<TempDirectory> temp_dir_;
    std::string saved_data_dir_;
    std::unique_ptr<Database> db_;
};

TEST_F(StatisticsDbTest, AnalyzeSingleTableReturnsMessage) {
    EXPECT_TRUE(db_->execute("CREATE TABLE emp (id INT, dept TEXT)").is_success());
    for (int i = 0; i < 50; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO emp VALUES (" + std::to_string(i) + ", 'eng')").is_success());

    auto r = db_->execute("ANALYZE TABLE emp");
    ASSERT_TRUE(r.message.has_value());
    EXPECT_EQ(*r.message, "ANALYZE");

    auto t = db_->get_catalog().lookup("emp");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->has_stats());
    EXPECT_EQ(t->table_stats().total_rows, 50u);
}

TEST_F(StatisticsDbTest, AnalyzeUnknownTableThrows) {
    EXPECT_THROW(db_->execute("ANALYZE TABLE nonexistent"), std::runtime_error);
}

TEST_F(StatisticsDbTest, AnalyzeAllTables) {
    EXPECT_TRUE(db_->execute("CREATE TABLE a (x INT)").is_success());
    EXPECT_TRUE(db_->execute("CREATE TABLE b (y INT)").is_success());
    EXPECT_TRUE(db_->execute("INSERT INTO a VALUES (1)").is_success());
    EXPECT_TRUE(db_->execute("INSERT INTO b VALUES (2)").is_success());

    auto r = db_->execute("ANALYZE");
    ASSERT_TRUE(r.message.has_value());
    EXPECT_EQ(*r.message, "ANALYZE");
    EXPECT_TRUE(db_->get_catalog().lookup("a")->has_stats());
    EXPECT_TRUE(db_->get_catalog().lookup("b")->has_stats());
}

TEST_F(StatisticsDbTest, ExplainShowsRowsAnnotation) {
    EXPECT_TRUE(db_->execute("CREATE TABLE t (id INT)").is_success());
    EXPECT_TRUE(db_->execute("CREATE INDEX idx_id ON t (id)").is_success());
    for (int i = 0; i < 40; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO t VALUES (" + std::to_string(i) + ")").is_success());

    // ANALYZE 前：无统计回退粗估仍输出 (rows=N)（SeqScan rows=40 来自 estimated_row_count）。
    auto r1 = db_->execute("EXPLAIN SELECT * FROM t");
    std::string plan1 = rows_to_text(r1);
    EXPECT_NE(plan1.find("(rows=40)"), std::string::npos) << plan1;

    // ANALYZE 后：过滤谓词的行数估计来自统计。
    EXPECT_TRUE(db_->execute("ANALYZE TABLE t").is_success());
    auto r2 = db_->execute("EXPLAIN SELECT * FROM t WHERE id < 10");
    std::string plan2 = rows_to_text(r2);
    EXPECT_NE(plan2.find("(rows="), std::string::npos) << plan2;
    EXPECT_NE(plan2.find("Index Scan"), std::string::npos) << plan2;
}

TEST_F(StatisticsDbTest, StatsPersistAcrossRestart) {
    EXPECT_TRUE(db_->execute("CREATE TABLE p (id INT)").is_success());
    for (int i = 0; i < 30; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO p VALUES (" + std::to_string(i) + ")").is_success());
    EXPECT_TRUE(db_->execute("ANALYZE TABLE p").is_success());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(temp_dir_->path()) / "p.stats"));

    // 重建 Database（同一数据目录）→ 统计自动加载。
    storage_internal::WalManager::instance().clear_all();
    db_.reset();
    db_ = std::make_unique<Database>(temp_dir_->path());
    auto t = db_->get_catalog().lookup("p");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->has_stats());
    EXPECT_EQ(t->table_stats().total_rows, 30u);
}

TEST_F(StatisticsDbTest, DropTableRemovesStatsFile) {
    EXPECT_TRUE(db_->execute("CREATE TABLE d (id INT)").is_success());
    EXPECT_TRUE(db_->execute("INSERT INTO d VALUES (1)").is_success());
    EXPECT_TRUE(db_->execute("ANALYZE TABLE d").is_success());
    const std::filesystem::path stats_file =
            std::filesystem::path(temp_dir_->path()) / "d.stats";
    EXPECT_TRUE(std::filesystem::exists(stats_file));

    EXPECT_TRUE(db_->execute("DROP TABLE d").is_success());
    EXPECT_FALSE(std::filesystem::exists(stats_file));
}

TEST_F(StatisticsDbTest, AutoAnalyzeTriggersOnStaleWrites) {
    EXPECT_TRUE(db_->execute("CREATE TABLE s (id INT)").is_success());
    for (int i = 0; i < 100; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO s VALUES (" + std::to_string(i) + ")").is_success());
    EXPECT_TRUE(db_->execute("ANALYZE TABLE s").is_success());

    auto t = db_->get_catalog().lookup("s");
    ASSERT_NE(t, nullptr);
    const uint64_t ts_before = t->table_stats().stats_ts;
    const std::size_t rows_at_analyze = t->table_stats().total_rows;
    EXPECT_EQ(t->rows_since_analyze(), 0u);

    // 追加 >10% 行：写入计数器累计；行数变化同样越阈。
    for (int i = 100; i < 130; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO s VALUES (" + std::to_string(i) + ")").is_success());
    EXPECT_EQ(t->rows_since_analyze(), 30u);

    // 下一次 SELECT 规划前触发 auto-ANALYZE：stats_ts 刷新、计数器清零。
    auto r = db_->execute("SELECT COUNT(*) FROM s");
    ASSERT_TRUE(r.rows.has_value());
    for (auto&& rec : *r.rows) { (void)rec; }

    EXPECT_NE(t->table_stats().stats_ts, ts_before);
    EXPECT_EQ(t->table_stats().total_rows, 130u);
    EXPECT_EQ(t->rows_since_analyze(), 0u);
    (void)rows_at_analyze;
}

TEST_F(StatisticsDbTest, AutoAnalyzeFirstQueryOnStatslessTable) {
    EXPECT_TRUE(db_->execute("CREATE TABLE f (id INT)").is_success());
    for (int i = 0; i < 20; ++i)
        EXPECT_TRUE(db_->execute("INSERT INTO f VALUES (" + std::to_string(i) + ")").is_success());

    // 无统计 → 首次 SELECT 触发采集。
    auto r = db_->execute("SELECT COUNT(*) FROM f");
    ASSERT_TRUE(r.rows.has_value());
    for (auto&& rec : *r.rows) { (void)rec; }

    auto t = db_->get_catalog().lookup("f");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->has_stats());
    EXPECT_EQ(t->table_stats().total_rows, 20u);
}
