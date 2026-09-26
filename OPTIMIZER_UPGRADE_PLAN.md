# CoroDB 查询优化器提升计划

> 目标：将 CoroDB 优化器从「启发式两段式重写器」推进到「商业级代价驱动优化器」。
> 评估基准：PostgreSQL / 金仓（KingbaseES）/ Oracle 的优化器能力栈。
> 编写日期：2026-08-13
>
> **实施进度（2026-09-27）**：
> - ✅ Phase 1 统计基础设施完成：ANALYZE 命令、.stats 持久化、MCV/等高直方图/NDV/null_frac 采集（NDV 采样升级为 MLE 估计）、[statistics] 配置节、选择性估计库（eq/range/in/is_null + AND/OR）、EXPLAIN (cost=..rows=) 注解、auto-ANALYZE（首次查询 + 10% 陈旧触发 + stats_ts 计划缓存指纹）。tests/test_statistics.cpp 26 用例。
> - ✅ Phase 2 统一代价模型完成：Cost{startup,total}（PG 对齐参数、[optimizer] 配置节）、全算子代价函数（SeqScan/IndexScan 含 correlation 修正/Filter/Sort/Top-N/Hash 与 Sort 聚合/Hash 与 Merge 与 NL 连接）、访问路径与连接算法代价比较（小表短路 + 无统计回退旧阈值 + cost_model 开关）。correlation 字段首次被消费。
> - ⬜ Phase 3+（DP 连接枚举、去关联、Hybrid Hash、并行）待实施。

---

## 一、现状评估

CoroDB 当前采用**两段式架构**，所有重写为纯启发式，物理选择为结构性规则 + 局部代价阈值。

### 1.1 架构

```
SQL AST ──> LogicalPlanner ──> LogicalPlan ──> RuleSet(5, fixed-point≤16) ──> PhysicalPlanner ──> PhysicalPlan
```

- [logical_planner.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/logical_planner.cpp)：AST → 逻辑计划树（Scan→Filter→Aggregate→Sort→Limit→Project）。
- [rule.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/optimizer/logical/rule.h)：`RuleSet` 固定点迭代框架。
- [physical_planner.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/physical/physical_planner.cpp)：逻辑节点 → 物理算子 + 算法选择。

### 1.2 已实现能力（值得保留的基础）

| 能力 | 实现位置 | 评价 |
|------|----------|------|
| 谓词下推（含 JOIN 分发） | [predicate_pushdown_rule.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/predicate_pushdown_rule.cpp) | 完整，正确 |
| 投影合并 | [projection_merge_rule.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/projection_merge_rule.cpp) | 基础可用 |
| 常量折叠 | [constant_folding_rule.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/constant_folding_rule.cpp) | 算术/字符串/NULL 传播 |
| 列裁剪 | [column_pruning_rule.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/column_pruning_rule.cpp) | 自顶向下传播 |
| Join 重排（小表左置） | [join_reorder_rule.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/join_reorder_rule.cpp) | **仅左右交换，左深树** |
| IndexScan 选择性代价 | [physical_planner.cpp#L191](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/physical/physical_planner.cpp#L191) `range_worth_index` / `equality_worth_index` | **proto-CBO**，已有 NDV/范围占比阈值 |
| Join 基数估算 | [join_reorder_rule.cpp#L18](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/join_reorder_rule.cpp#L18) `estimate_subtree_size` | **已有 L×R/NDV 乘积模型** |
| 算子选择 | `build_join`/`build_aggregate` | 结构性规则：等值→Hash、等值+排序→Merge、非等值→NL |

### 1.3 统计信息现状（关键短板）

[table.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h#L198) 暴露的统计接口：

| 方法 | 实现 | 问题 |
|------|------|------|
| `estimated_row_count()` | 存储引擎估算 / 内存行数 | 粗略 |
| `index_min_max(col)` | 有序索引首尾键 O(1) | 仅数值，仅索引列 |
| `index_distinct_count(col, cap)` | 遍历索引计数，达 cap 提前终止 | **仅索引列**；每次查询重算 |
| `index_range_fraction(...)` | 范围计数 + 提前退出 | **仅索引列**；运行时探针 |

**核心缺陷**：
1. **无持久化统计目录**——统计信息全部运行时从索引结构现算，无 `pg_statistic` 对应物。
2. **无直方图**——倾斜数据无法准确估算选择率。
3. **无 MCV（最常用值）**——少量高频值导致的选择率被均匀假设掩盖。
4. **非索引列零统计**——`index_distinct_count` 对无索引列返回 0，估算退化为保守上界。
5. **无 `ANALYZE` 命令**——统计不随数据变化刷新。
6. **无相关系数/多列统计**——无法处理列间相关性。

---

## 二、差距分析（vs 商业级）

按**对计划质量的影响程度**排序。商业级能力以 PostgreSQL 为对标。

### 🔴 关键差距（决定优化器上限）

#### G1. 无统一代价模型（Cost Model）
- **商业级**：`seq_page_cost` / `random_page_cost` / `cpu_tuple_cost` / `cpu_index_cost` / `parallel_*` 等可调参数；每个物理算子有 `cost()` 函数返回 (startup_cost, total_cost)。
- **CoroDB**：无统一代价。`range_worth_index`/`equality_worth_index` 是离散阈值（50%、NDV<4），`estimate_subtree_size` 只估行数不算代价。算子选择靠结构匹配，不比较代价。
- **影响**：HashJoin vs MergeJoin vs NLJoin 不做代价比较；IndexScan vs SeqScan 仅二值决策；无法表达「排序代价」「哈希表内存代价」「IO 代价」。

#### G2. Join 顺序仅左右交换，无 DP 枚举
- **商业级**：System-R 动态规划枚举左深树（≥12 表切换 GEQO 遗传算法），考虑 bushy 树，每对 join 候选都算代价选最优算法。
- **CoroDB**：[join_reorder_rule.cpp#L96](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/join_reorder_rule.cpp#L96) 仅对单个 INNER JOIN 节点做 `if (rsz < lsz) swap(left, right)`，固定点迭代收敛到局部最优。非 INNER JOIN 不重排。
- **影响**：3 表以上连接常陷次优序；无法生成 bushy 树；STAR schema 无法把事实表放中间。

#### G3. 无持久化统计 + 无直方图
- 见 §1.3。这是 G1/G2 估算准确性的地基。
- **影响**：所有基数/选择率估算在倾斜数据、范围谓词、多列谓词上误差极大，代价模型再准也是 GIGO。

### 🟠 主要差距（功能完整性）

#### G4. 相关子查询无去相关化
- **商业级**：`IN/EXISTS` → semi-join/anti-join；相关子查询 UNNEST 转 JOIN；magic-set 转换。
- **CoroDB**：[query_processor.cpp#L77](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp#L77) `substitute_outer_refs_*` 逐外层行代换后求值（apply 模型）。README 明确列为计划项。
- **影响**：相关子查询 O(N×M) 嵌套求值，无法走 JOIN 路径、无法并行、无法用 HashSemiJoin。

#### G5. Join 算法单一，无溢出
- **商业级**：Hybrid Hash Join（构建侧超内存则分片溢写、分批探测）、Grace Hash、Bloom-filter 加速的 Hash Join、Index Nested Loop（用内表索引探测外表行）。
- **CoroDB**：HashJoin 纯内存（大表 OOM 风险）；NLJoin 仅非等值场景的嵌套循环，无 index-NL 变体；无 Bloom filter。
- **影响**：大表 JOIN 要么 OOM 要么退化 NL；无法利用内表索引加速连接。

#### G6. 无并行执行
- **商业级**：Parallel SeqScan + Gather/Exchange、并行 Hash Join（build/probe 双侧并行）、并行 Aggregate（partial + final）。
- **CoroDB**：协程执行器单线程流水线。README 列为计划项。
- **影响**：无法利用多核；大查询吞吐受限。

### 🟡 中等差距（优化深度）

#### G7. IndexScan 形态有限
- **商业级**：Index-Only Scan（covering index，可见性映射免回表）、Bitmap Heap Scan + Bitmap AND/OR（多索引合并）、Index Skip Scan（低基数索引）、表达式索引、部分索引。
- **CoroDB**：等值/范围/IN/复合等值 IndexScan；超集索引逐 pk 点查 + 可见性重查（每次回表）。
- **影响**：`SELECT COUNT(*) FROM t WHERE indexed_col=?` 仍回表；多索引谓词无法合并。

#### G8. 聚合能力薄弱
- **商业级**：两阶段聚合（partial/final 支撑并行与溢出）、Distinct 聚合走排序、聚合溢写、GROUPING SETS/ROLLUP/CUBE。
- **CoroDB**：单阶段 Hash/Sort 聚合，无溢出，大基数千分组 OOM。
- **影响**：高基数 GROUP BY 易 OOM；无法并行聚合。

#### G9. 表达式与规则优化不足
- **商业级**：公共子表达式消除（CSE）、OR 展开为 UNION、谓词蕴含/矛盾推导（`x>10 AND x>5` → `x>10`）、约束推理（CHECK/NOT NULL/分区约束裁剪）、JIT 表达式编译。
- **CoroDB**：仅常量折叠。
- **影响**：冗余谓词不消除；约束信息未用于剪枝。

#### G10. 无运行时自适应
- **商业级**：Adaptive Join（运行时按实际行数切 Hash↔NL）、运行时统计反馈、计划自动校正、learned cardinality。
- **CoroDB**：静态计划，EXPLAIN ANALYZE 仅观测不改写。
- **影响**：估算错误导致的全表灾难计划无救济机制。

### 🟢 次要差距（易补齐）

#### G11. 计划管理薄弱
- **商业级**：Plan hints、plan baselines、绑定变量窥视、计划稳定性。
- **CoroDB**：LRU 计划缓存（128）+ DDL 失效；无 hint、无 baseline。
- **影响**：DBA 无法强制好计划；统计变化后计划可能突变变差。

#### G12. CTE/视图优化缺失
- **商业级**：视图合并、CTE inline vs materialize 代价决策、谓词下推进 CTE。
- **CoroDB**：CTE v1 基础支持，无合并/物化决策。
- **影响**：CTE 总被物化或总被 inline，非最优。

#### G13. LIMIT/TopN 下推有限
- **商业级**：Top-N sort 已有；LIMIT 下推穿过 JOIN（semi-join 早停）。
- **CoroDB**：Top-N 堆已实现（[physical_planner.cpp#L637](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/physical/physical_planner.cpp#L637)）；但 LIMIT 不下推穿过 JOIN。
- **影响**：`... JOIN ... LIMIT 10` 仍全量连接后裁剪。

---

## 三、提升计划（分阶段）

### 设计原则

1. **统计先行**——G1/G2 的准确性依赖 G3，必须最先做。
2. **不破坏现有架构**——在 `RuleSet` 与 `PhysicalPlanner` 之间插入代价层，而非推倒重来。
3. **保留启发式兜底**——CBO 失败或统计缺失时回退到现有规则，保证可用性。
4. **每阶段可独立验证**——配套 EXPLAIN 输出与单测。

---

### Phase 1：统计信息基础设施（解决 G3）

**目标**：建立持久化统计目录，支持直方图与 MCV，覆盖非索引列。

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 1.1 统计目录结构 | 新增 `include/corodb/storage/statistics.h` + `src/storage/statistics.cpp` | `ColumnStats{ null_frac, ndistinct, mcv[], mcv_freq[], histogram_bounds[], correlation }`；持久化到 `data/<table>.stats`（与 SSTable 同生命周期） |
| 1.2 `ANALYZE` 命令 | `src/sql/parser.cpp`（语法）+ `src/optimizer/physical/physical_planner.cpp`（`AnalyzePlan`） | `ANALYZE [TABLE t]`；扫描表采样（两阶段采样，默认 300×目标列数行）填充 `ColumnStats` |
| 1.3 Table 统计接口升级 | [table.h#L198](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/storage/table.h#L198) | 新增 `column_stats(col)`、`has_stats()`；`index_distinct_count` 优先读统计、回退现算 |
| 1.4 选择率函数库 | 新增 `include/corodb/optimizer/stats/selectivity.h` | `selectivity_eq/lt/gt/between/in(col, val, stats)`；等值 = MCV 命中或 `1/ndistinct`；范围 = 直方图分桶插值；AND=独立相乘、OR=容斥 |
| 1.5 自动 ANALYZE 触发 | `src/storage/table.cpp`（写入计数） | 行数变化超阈值（10%）自动后台 ANALYZE |
| 1.6 EXPLAIN 统计展示 | `src/process/explain_printer.cpp` | 计划节点附 `rows=...` 估算行数与 `selectivity=...` |

**验收**：`ANALYZE` 后 EXPLAIN 输出估算行数；倾斜列范围谓词估算误差 < 20%。

---

### Phase 2：统一代价模型 + 代价驱动物理选择（解决 G1）

**目标**：用代价比较替代离散阈值，所有物理算子有 `cost()` 函数。

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 2.1 代价参数 | `include/corodb/common/config.h`（新增 `[cost]` 段） | `seq_page_cost=1.0`、`random_page_cost=4.0`、`cpu_tuple_cost=0.01`、`cpu_index_cost=0.005`、`memory_per_op` |
| 2.2 CostModel | 新增 `include/corodb/optimizer/cost/cost_model.h` | `Cost{startup, total}`；各算子代价函数：SeqScan=`pages×seq_page_cost+rows×cpu_tuple_cost`；IndexScan=`random_page_cost×匹配页 + cpu_index_cost×rows`；HashJoin(build+probe)；MergeJoin(含排序代价)；NLJoin=`outer×inner_inner_cost` |
| 2.3 基数传播 | 新增 `src/optimizer/cost/cardinality_estimator.cpp` | 自底向上为每个节点计算 `estimated_rows` + `estimated_width`，复用 Phase 1 选择率 |
| 2.4 物理选择代价化 | [physical_planner.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/physical/physical_planner.cpp) | `build_join`/`build_filter` 改为枚举候选算子并取最小 `Cost`；`range_worth_index`/`equality_worth_index` 替换为代价比较 |
| 2.5 EXPLAIN 代价输出 | `src/process/explain_printer.cpp` | `cost=0.00..123.45 rows=100`（PG 风格） |

**验收**：相同查询在数据分布变化后，物理算子选择能随之改变；EXPLAIN 输出 PG 风格代价。

---

### Phase 3：DP 连接顺序枚举（解决 G2）

**目标**：替换 `JoinReorderRule` 的左右交换为 System-R 动态规划。

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 3.1 Join 图提取 | 新增 `src/optimizer/logical/join_order.cpp` | 从 Join 树提取关系集合 + 连接条件图（等值谓词形成 equi-join edges） |
| 3.2 DP 枚举 | 同上 | 左深树 DP（`best_plan[relset] = min over split`）；表数 ≤ 8 用 DP，>8 切遗传算法（GEQO 风格）或保留启发式兜底 |
| 3.3 每对 join 候选代价 | 复用 Phase 2 CostModel | 对每个候选 join 算 Hash/Merge/NL 三者代价取最小 |
| 3.4 Bushy 树支持 | 同 3.2 | DP 状态包含任意 relset 划分（可选，默认先左深） |
| 3.5 替换 R5 | [logical_planner.cpp 流水线](file:///c:/Users/jmh00/CLionProjects/corodb/src/optimizer/logical/logical_planner.cpp) | `JoinReorderRule` 降级为「表数≤3 时的快速兜底」，≥4 表走 DP |
| 3.6 外连接重排约束 | 同上 | 实现 outer-join 可交换性判定（保留语义正确性，参考 GRACEDB/PG `join_is_legal`） |

**验收**：4 表连接的 EXPLAIN 计划顺序与 PG 一致；TPC-H Q3/Q5 类查询计划合理。

---

### Phase 4：相关子查询去相关化（解决 G4）

**目标**：把相关 `IN/EXISTS` 转为 semi/anti-join，走 JOIN 优化路径。

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 4.1 Semi/Anti-Join 逻辑节点 | [logical_plan.h](file:///c:/Users/jmh00/CLionProjects/corodb/include/corodb/plan/logical_plan.h) | `LogicalJoin` 增加 `JoinType::Semi/Anti`；执行器实现 `SemiJoinPlan`/`AntiJoinPlan` |
| 4.2 去相关改写规则 | 新增 `src/optimizer/logical/unnest_subquery_rule.cpp` | `IN (SELECT ... WHERE outer.col=inner.col)` → `SemiJoin`；`EXISTS` 同理；`NOT IN/NOT EXISTS` → AntiJoin |
| 4.3 非相关子查询展开 | 同上 | 已部分支持（README），补全 `scalar subquery` in SELECT list → LateralJoin |
| 4.4 退化兜底 | [query_processor.cpp](file:///c:/Users/jmh00/CLionProjects/corodb/src/process/query_processor.cpp) | 无法去相关时保留现有 apply 求值，保证正确性 |

**验收**：相关 `EXISTS` 查询走 HashSemiJoin，EXPLAIN 不再出现逐行 apply。

---

### Phase 5：高级 Join/Aggregate 算法（解决 G5、G8）

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 5.1 Hybrid Hash Join | `src/executor/executor.cpp`（HashJoin 协程） | build 侧超内存预算时分片溢写临时文件，分批探测 |
| 5.2 Index Nested Loop | `physical_planner.cpp` `build_join` | 内表 join key 有索引时，外表逐行索引探测；代价纳入 Phase 2 |
| 5.3 Bloom Filter 加速 | 同上 | HashJoin build 侧建 Bloom filter，probe 侧提前过滤 |
| 5.4 聚合溢出 | `src/executor/executor.cpp`（HashAggregate） | 哈希表超内存时分片溢写 + 二次聚合 |
| 5.5 两阶段聚合（为 Phase 6 铺路） | 同上 | `PartialAggregate` + `FinalAggregate` 算子 |

**验收**：大表 JOIN 不再 OOM；Index NL 在合适场景被选中。

---

### Phase 6：并行查询执行（解决 G6）

| 子任务 | 目标文件/模块 | 交付物 |
|--------|---------------|--------|
| 6.1 Exchange/Gather 算子 | `include/corodb/plan/plan_node.h` + 执行器 | 协程模型上加并行扫描分片 |
| 6.2 Parallel SeqScan | `src/executor/executor.cpp` | 按 SSTable 区间分片，多 worker 并行 |
| 6.3 并行 Hash Join | 同上 | build 侧共享哈希表（或分片）+ probe 侧并行 |
| 6.4 并行 Aggregate | 复用 5.5 | partial 并行 + final 单点汇总 |
| 6.5 代价感知并行度 | Phase 2 CostModel | `parallel_setup_cost` + `parallel_tuple_cost`，决定是否并行 |

**验收**：大查询在多核上加速比 ≥ 2（4 核）。

---

### Phase 7：自适应与进阶能力（解决 G7、G9、G10、G11、G12、G13）

| 子任务 | 解决差距 | 交付物 |
|--------|----------|--------|
| 7.1 Index-Only Scan | G7 | covering index + 可见性映射，免回表 |
| 7.2 Bitmap Scan + Bitmap AND/OR | G7 | 多索引谓词合并 |
| 7.3 表达式简化 + CSE | G9 | 公共子表达式消除、冗余谓词消除、OR→UNION |
| 7.4 约束推理剪枝 | G9 | CHECK/NOT NULL 约束用于矛盾谓词短路 |
| 7.5 CTE/视图合并 | G12 | 代价决策 inline vs materialize |
| 7.6 LIMIT 下推穿 JOIN | G13 | semi-join 早停 |
| 7.7 Adaptive Join | G10 | 运行时首批行数观测，超阈值切 Hash→NL |
| 7.8 Plan Hint + Baseline | G11 | `/*+ HashJoin(t1 t2) */` 风格 hint；计划基线锁定 |

---

## 四、优先级与路线图

```
Phase 1 ──> Phase 2 ──> Phase 3 ──> Phase 4 ──┐
                                              │
                              Phase 5 ──> Phase 6 (并行)
                                              │
                                     Phase 7 (进阶，按需)
```

| Phase | 差距 | 工作量估 | 依赖 | 价值 |
|-------|------|----------|------|------|
| 1 统计基础设施 | G3 | 大 | 无 | ⭐⭐⭐⭐⭐ 地基 |
| 2 代价模型 | G1 | 中 | P1 | ⭐⭐⭐⭐⭐ |
| 3 DP 连接枚举 | G2 | 中 | P2 | ⭐⭐⭐⭐⭐ |
| 4 子查询去相关 | G4 | 中 | P2 | ⭐⭐⭐⭐ |
| 5 高级算法 | G5/G8 | 大 | P2 | ⭐⭐⭐⭐ |
| 6 并行执行 | G6 | 大 | P5 | ⭐⭐⭐ |
| 7 进阶 | G7/G9-G13 | 中 | P2 | ⭐⭐⭐ |

**建议起步顺序**：Phase 1 → 2 → 3 → 4。前三阶段完成后，优化器已具备商业级 CBO 的骨架（统计 + 代价 + 枚举），后续阶段在此基础上增量扩展。

---

## 五、风险与缓解

| 风险 | 缓解 |
|------|------|
| CBO 估算错误导致计划退化（比启发式更差） | 保留启发式兜底；`enable_cbo=off` 开关回退；Plan Baseline 锁定已知好计划 |
| 统计陈旧 | 自动 ANALYZE + 阈值触发；EXPLAIN 标注 `stats_stale` |
| DP 在大表数下爆炸 | 表数 ≤8 用 DP，>8 切 GEQO 遗传算法或保留现有 R5 |
| 协程模型改造并行难 | Phase 6 前先评估 `std::generator` 与并行 exchange 的兼容性，必要时引入 split/merge 点 |
| 外连接重排破坏语义 | 严格实现可交换性判定（参考 PG `make_join_rel` 合法性检查），单测覆盖 LEFT/RIGHT/FULL |

---

## 六、验收总标准

完成 Phase 1-4 后，CoroDB 应满足：

1. `ANALYZE` 后 EXPLAIN 输出 PG 风格 `rows`/`cost`/`selectivity`。
2. 4 表连接计划顺序由 DP 代价决定，非启发式交换。
3. 相关 `IN/EXISTS` 走 HashSemiJoin，非逐行 apply。
4. TPC-H（SF=1）查询计划与 PG 同方向，无明显次优（如大表 build HashJoin）。
5. 优化器单测覆盖率：统计选择率、代价函数、DP 枚举、去相关各 ≥ 20 用例。
