# Phase 1：统计信息基础设施 — 详细实现任务清单

> 关联文档：[OPTIMIZER_UPGRADE_PLAN.md §Phase 1](file:///c:/Users/jmh00/CLionProjects/corodb/OPTIMIZER_UPGRADE_PLAN.md)
> 目标：建立持久化列级统计目录，支持直方图 + MCV + NDV，覆盖所有列（含非索引列），为 Phase 2 代价模型提供准确选择率。

---

## 代码基线快照（实现前必读）

当前统计信息的全部实现集中在以下位置，所有新代码须与这些接口兼容：

| 现有接口 | 位置 | 行为 |
|----------|------|------|
| `Table::estimated_row_count()` | [table.cpp#L602](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L602) | 存储型走 `storage_->estimate_row_count()`，内存表走 `rows_.size()` |
| `Table::index_min_max(col)` | [table.cpp#L606](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L606) | 取有序索引 `indexes_[col]` 首尾键，O(1) |
| `Table::index_distinct_count(col, cap)` | [table.cpp#L613](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L613) | 遍历 `indexes_[col]` upper_bound 跳跃计数，达 cap 终止 |
| `Table::index_range_fraction(...)` | [table.cpp#L624](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L624) | 范围计数 / 总数，超 early_exit 短路 |
| `Table::indexes_` | [table.h#L257](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h#L257) | `unordered_map<col, multimap<Value, Value, ValueLess>>` |

**关键约束**：
- 统计仅覆盖索引列（`indexes_` 只有索引列的条目）
- 每次查询重算，不持久化
- `Value` = `variant<NullValue, int64_t, double, string>`（[types.h#L78](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/common/types.h#L78)）
- `Statement` variant 在 [ast.h#L744](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/ast/ast.h#L744)，新增 AST 节点需加至此
- Parser 分发在 [parser.cpp#L75](file:///c:/Users/jmh00/CLionProjects/corodb/src/sql/parser.cpp#L75)，命令执行在 [query_processor.cpp#L593](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp#L593)

---

## T1.1 — 统计数据结构定义

**目标**：定义 `ColumnStats` 结构，承载单列完整统计信息。

### 新增文件

| 文件 | 内容 |
|------|------|
| `include/corodb/storage/statistics.h` | 结构定义 + 序列化接口声明 |
| `src/storage/statistics.cpp` | 序列化/反序列化实现 |

### `ColumnStats` 结构设计

```cpp
// include/corodb/storage/statistics.h

namespace corodb {

    /** @brief 单列统计信息（ANALYZE 采集）。 */
    struct ColumnStats {
        // --- 基础 ---
        std::string column_name;
        TypeKind    type{ TypeKind::Null };
        std::size_t total_rows{ 0 };       // 采样时表总行数
        std::size_t null_count{ 0 };       // NULL 值数
        double      null_frac{ 0.0 };      // null_count / total_rows

        // --- 基数 ---
        std::size_t ndistinct{ 0 };        // 去重值数（精确或估算）

        // --- 最常用值 (MCV) ---
        std::vector<Value>  mcv_values;    // 高频值（降序排列）
        std::vector<double> mcv_freqs;     // 对应频率 (0..1]
        // MCV 仅存储 freq 显著高于 1/ndistinct 的值（PG 阈值：freq > 1.25 * avg_freq）

        // --- 等高直方图 ---
        // 排除 MCV 后的值按值域分桶，每桶代表约等量行
        std::vector<Value> histogram_bounds; // 桶边界，n_buckets+1 个值；空=无直方图
        // PG 默认桶数 = statistics_target（默认 100），CoroDB 先固定 32

        // --- 物理相关 ---
        double correlation{ 0.0 }; // 物理顺序与列值序的相关性 [-1,1]；影响 IndexScan 代价
                                 // 暂留字段，Phase 1 采集但不使用，Phase 2 代价模型消费

        // --- 元信息 ---
        uint64_t stats_ts{ 0 };   // 采集时的事务时间戳（用于判断是否陈旧）

        /** @brief 是否有有效统计。 */
        [[nodiscard]] bool valid() const noexcept {
            return total_rows > 0 || stats_ts > 0;
        }

        /** @brief 统计是否陈旧（行数变化超阈值）。 */
        [[nodiscard]] bool is_stale(std::size_t current_rows, double threshold = 0.1) const noexcept;

        /** @brief 序列化到二进制（用于持久化）。 */
        [[nodiscard]] std::string serialize() const;

        /** @brief 从二进制反序列化。 */
        static std::optional<ColumnStats> deserialize(const std::string& data);
    };

    /** @brief 单表全部列的统计集合。 */
    struct TableStats {
        std::string table_name;
        std::size_t total_rows{ 0 };
        uint64_t    stats_ts{ 0 };
        std::unordered_map<std::string, ColumnStats> columns;

        [[nodiscard]] const ColumnStats* column(const std::string& name) const;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::string serialize() const;
        static std::optional<TableStats> deserialize(const std::string& data);
    };

} // namespace corodb
```

### 序列化格式

```
TableStats 二进制格式：
+----------+----------+------------------+
| TSTAT(4) | Ver(1)   | table_name_len(2)|
+----------+----------+------------------+
| table_name(var) | total_rows(8) | stats_ts(8) |
+------------------+----------------+-----------+
| col_count(2) | ColumnStats[0] | ColumnStats[1] | ... |
+--------------+----------------+----------------+

ColumnStats 二进制格式：
+--------------+------------+----------+-----------+----------+
| col_name_len | col_name   | type(1)  | total(8)  | null(8)  |
+--------------+------------+----------+-----------+----------+
| ndistinct(8) | mcv_count(2) | [Value+freq(8)]×N |
+--------------+--------------+-------------------+
| hist_count(2) | Value×M | correlation(8) | stats_ts(8) |
+----------------+---------+----------------+-------------+

Value 编码：tag(1) + data
  tag=0x00 NULL  | tag=0x01 int64_t(8) | tag=0x02 string(len(4)+data) | tag=0x03 double(8)
```

**文件落盘路径**：`{data_dir}/{table_name}.stats`（与 SSTable 文件同目录，CHECKPOINT 时一并刷盘）。

### 验收标准
- [x] `ColumnStats` 可序列化/反序列化 round-trip 无损
- [x] 空统计 `deserialize` 返回 `nullopt` 不崩溃
- [x] 编译通过（加入 CMakeLists `add_library(corodb ...)`）

---

## T1.2 — 统计采集器（ANALYZE 核心逻辑）

**目标**：实现采样 + 统计计算逻辑，不涉及 SQL 语法（T1.3 负责）。

### 新增文件

| 文件 | 内容 |
|------|------|
| `include/corodb/storage/statistics_collector.h` | `StatisticsCollector` 类声明 |
| `src/storage/statistics_collector.cpp` | 采集实现 |

### 采集器接口

```cpp
// include/corodb/storage/statistics_collector.h

namespace corodb {

    class StatisticsCollector {
    public:
        /** @brief 配置参数。 */
        struct Config {
            std::size_t sample_target{ 300 };   // 每列目标采样行数（PG statistics_target × 3）
            std::size_t max_mcv{ 20 };          // MCV 最多保留个数
            std::size_t histogram_buckets{ 32 }; // 直方图桶数
            double      mcv_threshold{ 1.25 };   // MCV 频率阈值倍数（vs 1/ndistinct）
        };

        explicit StatisticsCollector(Config cfg = {}) : cfg_(std::move(cfg)) {}

        /** @brief 对单表采集统计。 */
        [[nodiscard]] TableStats collect(const Table& table, uint64_t snapshot_ts) const;

    private:
        Config cfg_;

        // --- 单列采集 ---
        [[nodiscard]] ColumnStats collect_column(
            const Table& table, const std::string& col_name,
            TypeKind col_type, uint64_t snapshot_ts) const;

        // --- 采样：两阶段 Bernoulli 采样 ---
        // 阶段1：按 sample_target / total_rows 算采样率 p，流式 Bernoulli 采样
        // 阶段2：若 p > 1（小表）则全量；若 total_rows 未知（存储型）则固定 N 行
        void sample_column_values(
            const Table& table, std::size_t col_idx,
            uint64_t snapshot_ts, std::vector<Value>& out) const;

        // --- MCV 计算 ---
        void compute_mcv(
            std::vector<Value>& samples, std::size_t ndistinct,
            std::vector<Value>& mcv_values, std::vector<double>& mcv_freqs) const;

        // --- 直方图计算（排除 MCV 后的值） ---
        void compute_histogram(
            std::vector<Value>& remaining, TypeKind type,
            std::vector<Value>& bounds) const;

        // --- NDV 估算（采样集去重 + 外推） ---
        [[nodiscard]] std::size_t estimate_ndv(
            const std::vector<Value>& samples, std::size_t sample_size,
            std::size_t total_rows) const;
    };

} // namespace corodb
```

### 采集算法细节

**采样策略**：
- 利用 [Table::scan_visible_stream(snapshot_ts)](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L263) 流式扫描，避免全量物化
- 小表（`estimated_row_count() <= sample_target × 3`）：全量扫描
- 大表：Bernoulli 采样，每行以概率 `p = sample_target / estimated_row_count` 纳入样本

**MCV 计算**：
1. 用 `unordered_map<Value, size_t, ValueHash, ValueEq>` 统计样本中各值频次
2. 计算平均频率 `avg_freq = 1.0 / ndistinct`
3. 保留频次 `> mcv_threshold × avg_freq` 的前 `max_mcv` 个值
4. 频率外推：`mcv_freq = sample_count / total_rows`（全量扫描时精确）

**直方图计算**（等高直方图）：
1. 从样本中移除 MCV 值
2. 按值排序（使用 `ValueLess`）
3. 分成 `histogram_buckets` 个等高桶
4. 记录桶边界（`bounds[0] = min, bounds[N] = max`）
5. 仅对可排序类型（int64/double/string）建直方图；NULL 类型跳过

**NDV 估算**：
- 全量扫描：精确去重计数
- 采样：`ndistinct = sample_ndistinct + (total_rows - sample_size) × (sample_ndistinct / sample_size) × correction_factor`
- `correction_factor` 默认 1.0（保守），后续可换 Charikar 估计

### 验收标准
- [x] 10 万行均匀分布整数列：NDV 估算误差 < 15%
- [x] 倾斜列（90% 值=1，10% 随机）：MCV 捕获值=1，freq ≈ 0.9
- [x] NULL 比例准确
- [x] 采集过程不 OOM（流式采样，不物化全表）

---

## T1.3 — ANALYZE SQL 命令（语法 + AST + 执行）

**目标**：支持 `ANALYZE [TABLE name]` 与 `ANALYZE`（全表）。

### 改动清单

#### 3a. AST 节点 — [ast.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/ast/ast.h)

在 `SetTransactionStmt` 之后、`Statement` variant 之前新增：

```cpp
/** @struct AnalyzeStmt @brief ANALYZE 语句，采集表统计信息。 */
struct AnalyzeStmt {
    std::string table_name;  ///< 目标表名；空 = 所有表
};
```

在 `Statement` variant（[ast.h#L744](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/ast/ast.h#L744)）末尾追加 `AnalyzeStmt`：

```cpp
using Statement = std::variant<..., SetTransactionStmt, AnalyzeStmt, std::shared_ptr<ExplainStmt>>;
```

#### 3b. Parser — [parser.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/sql/parser.h) + [parser.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/sql/parser.cpp)

**parser.h**：在 `parse_set()` 声明后追加：
```cpp
/// 解析 ANALYZE 语句。
[[nodiscard]] AnalyzeStmt parse_analyze();
```

**parser.cpp** 分发（[parser.cpp#L113](file:///c:/Users/jmh00/CLionProjects/corodb/src/sql/parser.cpp#L113)，`SET` 分支后）：
```cpp
if (head == "ANALYZE")
    return parse_analyze();
```

**parser.cpp** 实现（在 `parse_set()` 函数后）：
```cpp
AnalyzeStmt Parser::parse_analyze() {
    consume(); // consume ANALYZE
    AnalyzeStmt stmt;
    // 可选 TABLE 关键字
    if (match_keyword("TABLE")) {
        stmt.table_name = consume_identifier();
    } else if (peek_kind() == TokenType::Identifier) {
        stmt.table_name = consume_identifier();
    }
    // ANALYZE 无表名 = 分析所有表
    return stmt;
}
```

#### 3c. 执行路径 — [query_processor.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp)

在 `ShowStatusStmt` 处理（[query_processor.cpp#L601](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp#L601)）后新增：

```cpp
if (std::holds_alternative<AnalyzeStmt>(stmt)) {
    const auto& a = std::get<AnalyzeStmt>(stmt);
    if (a.table_name.empty()) {
        // 分析所有表
        for (const auto& name : catalog_.table_names())
            analyze_table(*catalog_.lookup(name));
    } else {
        auto t = catalog_.lookup(a.table_name);
        if (!t)
            throw std::runtime_error("[ANALYZE] Unknown table: " + a.table_name);
        analyze_table(*t);
    }
    ProcessedQuery q;
    q.message = "ANALYZE";
    return q;
}
```

`analyze_table` 私有方法需要访问 `StatisticsCollector` 和 `TableStats` 持久化（见 T1.4）。

#### 3d. 计划缓存失效

`ANALYZE` 不改变数据但改变统计，须在 [query_processor.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp) 中 ANALYZE 执行后清空计划缓存（与 DDL 同路径），防止旧计划继续使用陈旧统计。

### 验收标准
- [x] `ANALYZE TABLE employees;` 返回 `ANALYZE`
- [x] `ANALYZE;` 分析所有表
- [x] `ANALYZE nonexistent;` 报错
- [x] ANALYZE 后计划缓存被清空
- [x] `EXPLAIN ANALYZE TABLE employees;` 不误触发统计采集（ExplainStmt 包裹）

---

## T1.4 — 统计持久化与 Table 接口升级

**目标**：统计信息持久化到磁盘 + Table 暴露统计查询接口。

### 改动清单

#### 4a. Table 持有统计 — [table.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h) + [table.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp)

**table.h** 新增 `#include "corodb/storage/statistics.h"`，在 private 成员区追加：

```cpp
private:
    TableStats stats_;  // 列级统计缓存
    bool stats_loaded_{ false };
```

在 public 接口区（`index_range_fraction` 之后，[table.h#L216](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h#L216)）追加：

```cpp
public:
    /** @brief 获取列统计信息；无统计返回 nullptr。 */
    [[nodiscard]] const ColumnStats* column_stats(const std::string& col) const;

    /** @brief 是否已加载统计信息。 */
    [[nodiscard]] bool has_stats() const noexcept { return stats_loaded_ && stats_.valid(); }

    /** @brief 加载持久化统计（从 {data_dir}/{name}.stats）；文件不存在则标记无统计。 */
    void load_stats(const std::string& data_dir);

    /** @brief 持久化统计到 {data_dir}/{name}.stats。 */
    void save_stats(const std::string& data_dir) const;

    /** @brief 更新统计（ANALYZE 调用）。 */
    void update_stats(TableStats new_stats);
```

#### 4b. 统计加载时机

在 `Table` 构造函数（[table.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp) Table 构造函数处）中，当 `storage_ != nullptr`（存储型表）时，尝试 `load_stats(storage_->data_dir())`。文件不存在不报错，静默标记无统计。

#### 4c. 现有统计方法优先读 stats_

修改 [table.cpp#L613](file:///c:/Users/jmh00/CLionProjects/corodb/src/storage/table.cpp#L613) `index_distinct_count`：

```cpp
std::size_t Table::index_distinct_count(const std::string& column, std::size_t cap) const {
    // 优先读持久化统计
    if (auto* s = column_stats(column); s && s->ndistinct > 0)
        return std::min(s->ndistinct, cap);
    // 回退：从索引结构现算（保持向后兼容）
    auto it = indexes_.find(column);
    // ... 现有逻辑不变 ...
}
```

同理修改 `index_min_max`：优先从 `ColumnStats::histogram_bounds` 的首尾取 min/max。

`index_range_fraction` **不改**（它是运行时精确探针，Phase 2 代价模型用直方图选择率替代它，但保留作为 fallback）。

#### 4d. CMakeLists 注册

在 [CMakeLists.txt](file:///c:/Users/jmh00/CLionProjects/corodb/CMakeLists.txt#L50) 存储引擎段追加：

```cmake
# 存储引擎
src/storage/storage_engine_base.cpp
src/storage/storage_engine_common.cpp
src/storage/table.cpp
src/storage/buffer_pool.cpp
src/storage/lsm_storage_engine.cpp
src/storage/statistics.cpp            # 新增
src/storage/statistics_collector.cpp  # 新增
```

### 验收标准
- [x] ANALYZE 后重启数据库，统计仍可加载
- [x] `column_stats("non_indexed_col")` 返回非 nullptr（核心改进：覆盖非索引列）
- [x] `index_distinct_count` 在有统计时走统计、无统计时走索引回退
- [x] DROP TABLE 时 `.stats` 文件一并删除（在 DropTablePlan 中处理）

---

## T1.5 — 选择率函数库

**目标**：为 Phase 2 代价模型提供统一的选择率计算接口。

### 新增文件

| 文件 | 内容 |
|------|------|
| `include/corodb/optimizer/stats/selectivity.h` | 选择率函数声明 |
| `src/optimizer/stats/selectivity.cpp` | 实现 |

### 接口设计

```cpp
// include/corodb/optimizer/stats/selectivity.h

namespace corodb::opt {

    /** @brief 选择率计算器（基于 ColumnStats）。 */
    class SelectivityEstimator {
    public:
        /** @brief 等值选择率 col = val。 */
        static double eq(const ColumnStats* stats, const Value& val);

        /** @brief 范围选择率 col < val / col <= val / col > val / col >= val。 */
        static double range(const ColumnStats* stats, CompareOp op, const Value& val);

        /** @brief BETWEEN 选择率 col BETWEEN low AND high。 */
        static double between(const ColumnStats* stats, const Value& low, const Value& high);

        /** @brief IN 选择率 col IN (v1, ..., vN)。 */
        static double in_list(const ColumnStats* stats, const std::vector<Value>& values);

        /** @brief IS NULL 选择率。 */
        static double is_null(const ColumnStats* stats);

        /** @brief IS NOT NULL 选择率。 */
        static double is_not_null(const ColumnStats* stats);

        /** @brief AND 合并（假设独立）：s1 * s2。 */
        static double combined_and(double s1, double s2) { return s1 * s2; }

        /** @brief OR 合并（容斥）：s1 + s2 - s1*s2。 */
        static double combined_or(double s1, double s2) { return s1 + s2 - s1 * s2; }

        /** @brief NOT 合并：1 - s。 */
        static double combined_not(double s) { return 1.0 - s; }

    private:
        // 直方图分桶定位：返回 val 所在桶的 [0, 1) 位置
        static double histogram_position(const std::vector<Value>& bounds, const Value& val);

        // 无统计时的保守默认值
        static constexpr double kDefaultEqSelectivity{ 0.005 };     // 1/200
        static constexpr double kDefaultRangeSelectivity{ 0.333 };  // 1/3
        static constexpr double kDefaultNullFrac{ 0.0 };
    };

} // namespace corodb::opt
```

### 算法细节

**等值选择率 `eq(stats, val)`**：
1. 若 `stats == nullptr` → 返回 `kDefaultEqSelectivity`（0.5%）
2. 若 `stats->mcv_values` 非空 → 遍历 MCV，命中则返回对应 `mcv_freqs[i]`
3. 未命中 MCV → `(1 - sum(mcv_freqs)) / (ndistinct - mcv_count)`
4. 无 MCV 但有 NDV → `1.0 / ndistinct`（含 NULL 时 `(1 - null_frac) / ndistinct`）

**范围选择率 `range(stats, op, val)`**：
1. 若 `stats == nullptr` → 返回 `kDefaultRangeSelectivity`（1/3）
2. 若有直方图 → `histogram_position(bounds, val)` 定位桶内位置，线性插值
3. 无直方图但有 min/max → 线性插值（复用现有 `range_worth_index` 逻辑）
4. 均无 → 返回 `kDefaultRangeSelectivity`

**IN 选择率 `in_list(stats, values)`**：
1. 对每个 value 调用 `eq`，求和后 cap 到 1.0
2. 注意去重（相同值只算一次）

### 验收标准
- [x] 均匀分布 1000 值列，等值选择率 ≈ 0.001
- [x] MCV 中值的选择率精确返回高频
- [x] 范围 `> p50` 的选择率 ≈ 0.5（直方图分桶准确）
- [x] 无统计时不崩溃，返回合理默认值

---

## T1.6 — 配置支持

**目标**：新增 `[statistics]` 配置段。

### 改动清单

#### 6a. Config 类 — [config.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/common/config.h)

在 `[auth]` 段之后新增 `[statistics]` 段：

```cpp
// =========================================================================
// [statistics]
// =========================================================================
[[nodiscard]] std::size_t stats_sample_target() const noexcept { return stats_sample_target_; }
[[nodiscard]] std::size_t stats_max_mcv() const noexcept { return stats_max_mcv_; }
[[nodiscard]] std::size_t stats_histogram_buckets() const noexcept { return stats_histogram_buckets_; }
[[nodiscard]] double stats_auto_analyze_threshold() const noexcept { return stats_auto_analyze_threshold_; }
void set_stats_sample_target(std::size_t v) noexcept { stats_sample_target_ = v; }
void set_stats_max_mcv(std::size_t v) noexcept { stats_max_mcv_ = v; }
void set_stats_histogram_buckets(std::size_t v) noexcept { stats_histogram_buckets_ = v; }
void set_stats_auto_analyze_threshold(double v) noexcept { stats_auto_analyze_threshold_ = v; }
```

private 成员：
```cpp
// [statistics]
std::size_t stats_sample_target_ = 300;
std::size_t stats_max_mcv_ = 20;
std::size_t stats_histogram_buckets_ = 32;
double      stats_auto_analyze_threshold_ = 0.1; // 10% 行数变化触发自动 ANALYZE
```

#### 6b. Config 解析 — [config.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/common/config.cpp)

在 `apply_kv`（[config.cpp#L118](file:///c:/Users/jmh00/CLionProjects/corodb/src/common/config.cpp#L118)）末尾追加：

```cpp
// statistics.*
if (try_set_size("statistics.sample_target", &Config::stats_sample_target_)) return;
if (try_set_size("statistics.max_mcv", &Config::stats_max_mcv_)) return;
if (try_set_size("statistics.histogram_buckets", &Config::stats_histogram_buckets_)) return;
if (k == "statistics.auto_analyze_threshold") {
    // 解析 double
    try { stats_auto_analyze_threshold_ = std::stod(value); } catch (...) {}
    return;
}
```

#### 6c. 默认配置生成 — [genconfig.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/server/genconfig.cpp)

在配置模板中追加：

```ini
[statistics]
# 每列目标采样行数（PG statistics_target × 3）
sample_target = 300
# MCV 最多保留个数
max_mcv = 20
# 直方图桶数
histogram_buckets = 32
# 自动 ANALYZE 触发阈值（行数变化比例）
auto_analyze_threshold = 0.1
```

### 验收标准
- [x] 配置文件中 `[statistics]` 段可被正确加载
- [x] 未配置时使用默认值
- [x] `genconfig` 生成的配置文件包含 `[statistics]` 段

---

## T1.7 — EXPLAIN 统计展示

**目标**：EXPLAIN 输出中附加估算行数和选择率（PG 风格）。

### 改动清单

#### 7a. PlanNode 增加 CardinalityEstimate — [physical_plan.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/plan/physical_plan.h)

在 `PlanNode` 基类（[physical_plan.h#L91](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/plan/physical_plan.h#L91)）中追加：

```cpp
struct PlanNode {
    // ... 现有代码 ...

    /** @brief 优化器估算的输出行数（Phase 1 填充，Phase 2 代价模型消费）。 */
    double estimated_rows{ 0.0 };

    /** @brief 估算的选择率（仅 Filter/Join 有意义）。 */
    double estimated_selectivity{ 1.0 };

    // ... 其余不变 ...
};
```

#### 7b. ExplainPrinter 输出 — [explain_printer.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/explain_printer.cpp)

在每个算子行后追加 `(rows=N)`，在 Filter 后追加 `(selectivity=S)`：

```
Seq Scan on employees (rows=5000)
  ->  Filter: (dept = 'Engineering') (selectivity=0.02) (rows=100)
```

**实现方式**：修改 explain_printer 中各 `print_*` 函数，读取 `node.estimated_rows` 和 `node.estimated_selectivity`，非零时附加输出。

#### 7c. 物理规划器填充估算 — [physical_planner.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/physical/physical_planner.cpp)

在 `build_scan` / `build_filter` / `build_join` 中填充 `estimated_rows`：
- `build_scan`：`plan->estimated_rows = table->estimated_row_count()`
- `build_filter`：`plan->estimated_rows = child->estimated_rows × selectivity`（用 T1.5 的 SelectivityEstimator）
- `build_join`：复用 [join_reorder_rule.cpp#L18](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/join_reorder_rule.cpp#L18) `estimate_subtree_size` 的乘积模型

**注意**：Phase 1 仅做行数估算填充，不做代价比较（Phase 2 的任务）。此处填充为 Phase 2 铺路。

### 验收标准
- [x] `EXPLAIN SELECT * FROM employees WHERE dept='Eng'` 输出含 `(rows=N)`
- [x] ANALYZE 后估算行数更准确
- [x] 无统计时 `(rows=N)` 仍输出（回退粗估）

---

## T1.8 — 自动 ANALYZE 触发

**目标**：写入操作导致行数变化超阈值时自动后台 ANALYZE。

### 改动清单

#### 8a. Table 写计数器 — [table.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h)

Table 已有 `write_counter_`（[table.h#L250](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h#L250)），但用于 Serializable 幻读检测。新增独立统计刷新计数器：

```cpp
private:
    std::atomic<std::size_t> rows_since_analyze_{ 0 }; // 自上次 ANALYZE 以来的写入行数
    std::size_t rows_at_last_analyze_{ 0 };             // 上次 ANALYZE 时的表行数
```

在 `insert` / `insert_batch` 中递增 `rows_since_analyze_`。
在 `update_stats` 中记录 `rows_at_last_analyze_ = stats_.total_rows`，重置 `rows_since_analyze_ = 0`。

#### 8b. 触发判定 — [query_processor.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp)

在 DML 执行后检查：

```cpp
// DML 执行后
if (table->rows_since_analyze() > 0) {
    double change_ratio = static_cast<double>(table->rows_since_analyze()) /
                          std::max<std::size_t>(table->rows_at_last_analyze(), 1);
    if (change_ratio > Config::instance().stats_auto_analyze_threshold()) {
        // 标记需要 ANALYZE，异步执行或下条语句前同步执行
        pending_analyze_tables_.insert(table->name());
    }
}
```

**策略**：Phase 1 采用同步延迟 ANALYZE——在事务提交后、返回客户端前，检查并执行 pending ANALYZE。避免引入后台线程的复杂性。后续可改为后台线程。

#### 8c. 首次查询无统计时自动触发

若表无统计（`!has_stats()`）且行数 > 0，首次查询该表时触发一次 ANALYZE。

### 验收标准
- [x] 插入 10% 行数后下次查询自动 ANALYZE
- [x] 首次查询无统计表触发 ANALYZE
- [x] 自动 ANALYZE 不阻塞写入（同步实现时 ANALYZE 在 DML 之后）

---

## T1.9 — 测试

**目标**：覆盖统计采集、序列化、选择率、EXPLAIN 输出。

### 新增文件

| 文件 | 内容 |
|------|------|
| `tests/test_statistics.cpp` | 统计采集 + 序列化 + 选择率测试 |

### 测试用例清单

```
// --- 序列化 round-trip ---
TEST(Statistics, ColumnStatsSerializeRoundTrip)
TEST(Statistics, TableStatsSerializeRoundTrip)
TEST(Statistics, DeserializeEmptyReturnsNullopt)

// --- 采集器 ---
TEST(Statistics, CollectUniformIntColumn)        // 均匀分布：NDV 误差 < 15%
TEST(Statistics, CollectSkewedColumn)             // 倾斜：MCV 捕获高频值
TEST(Statistics, CollectNullValues)               // NULL 比例准确
TEST(Statistics, CollectStringColumn)             // 字符串列直方图
TEST(Statistics, CollectSmallTable)               // 小表全量扫描
TEST(Statistics, CollectLargeTableSampling)       // 大表采样不 OOM

// --- 选择率 ---
TEST(Selectivity, EqWithMCV)                      // MCV 命中返回精确频率
TEST(Selectivity, EqWithoutMCV)                   // 无 MCV 用 1/ndistinct
TEST(Selectivity, EqNoStats)                      // 无统计返回默认值
TEST(Selectivity, RangeWithHistogram)             // 直方图插值
TEST(Selectivity, RangeNoStats)                   // 无统计返回 1/3
TEST(Selectivity, Between)                        // BETWEEN = range 之差
TEST(Selectivity, InList)                         // IN 求和 cap 1.0
TEST(Selectivity, AndOrNot)                       // 逻辑组合

// --- 持久化 ---
TEST(Statistics, PersistAndReload)                // 写盘后重启可加载
TEST(Statistics, DropTableRemovesStats)           // DROP TABLE 删除 .stats

// --- EXPLAIN ---
TEST(Explain, ShowsEstimatedRows)                 // EXPLAIN 输出含 rows=N
TEST(Explain, ShowsSelectivity)                   // Filter 输出 selectivity

// --- ANALYZE 命令 ---
TEST(AnalyzeCommand, SingleTable)                 // ANALYZE TABLE t
TEST(AnalyzeCommand, AllTables)                   // ANALYZE
TEST(AnalyzeCommand, UnknownTable)                // 报错
TEST(AnalyzeCommand, ClearsPlanCache)             // 执行后计划缓存清空

// --- 自动触发 ---
TEST(AutoAnalyze, TriggersOnThreshold)            // 10% 变化触发
TEST(AutoAnalyze, TriggersOnFirstQuery)           // 首次查询无统计触发
```

### CMakeLists 注册

在 [CMakeLists.txt](file:///c:/Users/jmh00/CLionProjects/corodb/CMakeLists.txt#L166) 追加：

```cmake
add_module_test(statistics)
```

并在 `corodb_tests` 源文件列表中追加 `tests/test_statistics.cpp`。

### 验收标准
- [x] 全部测试通过
- [x] 均匀分布 NDV 估算误差 < 15%
- [x] 倾斜列 MCV 正确捕获
- [x] 序列化 round-trip 无损

---

## 实现顺序与依赖

```
T1.1 (结构定义) ──────┐
                      ├──> T1.2 (采集器) ──> T1.3 (ANALYZE命令) ──> T1.4 (持久化+Table接口)
                      │                                                         │
                      │                                                         v
T1.5 (选择率库) <─────┘                                                   T1.7 (EXPLAIN展示)
                      │                                                         │
T1.6 (配置) ──────────┘                                                    T1.8 (自动触发)
                                                                        │
                                                                        v
                                                                    T1.9 (测试)
```

**建议实现顺序**：

| 步骤 | 任务 | 依赖 | 可验证方式 |
|------|------|------|------------|
| 1 | T1.1 结构定义 | 无 | 编译通过 |
| 2 | T1.6 配置 | 无 | 配置加载单测 |
| 3 | T1.2 采集器 | T1.1 | 独立采集单测（不经 SQL） |
| 4 | T1.4 持久化 + Table 接口 | T1.1, T1.2 | 序列化 round-trip 单测 |
| 5 | T1.3 ANALYZE 命令 | T1.2, T1.4 | `ANALYZE TABLE t` 端到端 |
| 6 | T1.5 选择率库 | T1.1 | 选择率单测 |
| 7 | T1.7 EXPLAIN 展示 | T1.4, T1.5 | EXPLAIN 输出含 rows= |
| 8 | T1.8 自动触发 | T1.3 | 插入后自动 ANALYZE |
| 9 | T1.9 完整测试 | 全部 | ctest 全绿 |

---

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 采样导致统计偏差 | 小表全量扫描；大表采样 + NDV 外推校正因子 |
| 序列化格式变更导致不兼容 | 版本号字段（`Ver(1)`），反序列化检查版本 |
| 自动 ANALYZE 阻塞写入 | Phase 1 同步实现但仅 DML 后触发；Phase 6 改后台线程 |
| 现有测试因 `estimated_rows` 默认值失败 | 默认值 0.0，EXPLAIN 仅在非零时输出 |
| `index_distinct_count` 行为变化 | 有统计时走统计（更准），无统计时回退原逻辑（行为不变） |
