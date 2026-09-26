# CoroDB 生产就绪度提升路线图

> 现状：设计精良的教学/作品级数据库（~2 万行 C++23），完整演示 LSM-Tree、MVCC、
> Volcano 协程执行、规则优化器、Multi-Reactor 网络。距离"生产可用"仍有架构级差距。
> 本路线图按依赖顺序推进：先保证不丢/不错数据 → 再打破扩展性天花板 → 再补安全运维 →
> 最后 SQL 能力与高可用。

## P0 — 正确性与持久性

- [x] **WAL 事务边界 + 崩溃原子恢复**：全局提交日志（commit log）+ 提交感知恢复，
  提交在崩溃后**原子生效**（含跨表），撕裂写入被丢弃；旧格式 WAL 向后兼容。
- [x] **跨表事务原子性**：单一全局提交日志作为原子提交点，同一 commit_ts 跨表全有或全无。
- [x] 崩溃恢复回归测试：`LSMEngineTest.CrashRecovery{KeepsBarrierSealedCommit,DiscardsTornCommit,CrossTableAtomicity}`。
- [x] 服务器默认 `wal.sync_mode = durable`（断电不丢已提交数据）。
- [x] **修复 SSTable 冷读数据损坏**：数据页为无页头的原始 payload，读路径曾把 payload
  字节误判为页校验和，导致刷盘后（>1MB 或 CHECKPOINT）已提交数据不可读——已移除该误判校验。
- [x] 提交日志 GC：checkpoint 覆盖所有磁盘表后安全截断（`CheckpointTruncatesCommitLogKeepsCommittedData`）。
- [x] 写入路径强制约束：NOT NULL / 类型校验 / 主键唯一性（`TxnTest.*`；约束标志持久化且向后兼容）。
- [ ] SSTable/Compaction 引入 MANIFEST，避免压缩中途崩溃留下不一致文件集。
- [ ] 真正的页级校验和：为 SSTable 数据页预留页头并 stamp/verify（检测磁盘静默损坏）。

## P1 — 打破内存天花板（最大工程量）

- [x] **执行器流式读存储，移除 `Table::rows_` 全量常驻**：存储型表不再构造时加载整表；
  SeqScan/COUNT/UPDATE/DELETE/事务提交/CREATE INDEX 均经 `scan_visible`/`persist_*` 走存储；`rows_` 仅留给无存储的测试夹具。
- [x] **二级索引改为 `(列值 → 主键)`**：与 `rows_` 位置解耦；IndexScan 改用 `lookup_visible(pk)`
  + 可见性/值重查（“超集 + 重查”，MVCC 下正确，含索引列被更新/删除）；索引追加改为 O(1)。
- [x] **范围索引扫描**（`col >/>=/</<= v`）：有序 multimap 索引 + range lookup + 可见性重查；EXPLAIN 显示范围条件。
- [x] **BETWEEN / IN 走索引**：BETWEEN→双侧范围扫描、IN→多等值点查并集，均带可见性重查与 EXPLAIN 呈现。
- [x] **多列（复合）等值索引**：`CREATE INDEX idx ON t (a, b, ...)`；`a=? AND b=?` 合取命中复合 IndexScan
  （可带残差 Filter），超集 + 可见性重查，定义经注册表持久化、重启恢复。复合范围/前缀匹配与索引超集压缩待做。
- [x] **主键泛化（任意标量 + 复合）**：主键 int64→Value（int64/字符串/浮点），复用 write_value 编码 + ValueLess 排序，
  贯穿 memtable/WAL/SSTable/Bloom/锁/会话/执行器；≥2 个 PRIMARY KEY 列时为复合主键（各 PK 列 encode_key
  拼接的字符串键，对存储/锁/缓冲透明；取键点统一收敛到 extract_key(row, schema)/Table::row_key）。
- [x] **流式扫描 + 惰性 SSTable 页读取**：SeqScan 经 `scan_visible_stream` 逐行产出（k 路 MVCC 归并，不物化结果集）；
  SSTable 按需经 Buffer Pool pin 页解码，不再整表解码——读路径真正承载大于内存的数据集。

## P2 — 安全与运维

- [ ] 强制认证 + Argon2/bcrypt/scrypt 密码哈希。
- [ ] TLS 传输加密。
- [ ] 授权 / RBAC + 审计日志。
- [ ] Prometheus 指标、健康检查、慢查询日志。
- [ ] 备份 / 恢复工具链（一致性快照、增量、PITR）。

## P3 — SQL 能力与优化器

- [x] **非相关 IN/EXISTS 子查询**：WHERE 中 `[NOT] IN (SELECT ...)` 与 `[NOT] EXISTS (SELECT ...)`
  （SELECT/UPDATE/DELETE，可嵌套）；先执行子查询代换（IN→字面量列表可命中 IndexScan，EXISTS→恒真/假，
  存在性短路）再规划，数据相关故跳过计划缓存。
- [x] **NULL 字面量 + IS NULL 执行**：`INSERT/UPDATE/WHERE` 中的 `NULL` 字面量（保留字，不作列名）；
  IS NULL/IS NOT NULL 求值与 NOT NULL 约束早已就绪，缺口实为字面量解析。
- [x] **相关子查询（IN/EXISTS）**：引用外层列（须表名/别名限定）的子查询保留到执行期，
  由 SubqueryRunner 逐外层行代换引用为字面量后递归规划/执行（nested apply，O(N×M) 正确优先；
  去相关化为后续优化）；共享 AST 节点克隆后再代换，嵌套作用域正确。
- [x] **UNION / UNION ALL**：`SELECT ... UNION [ALL] SELECT ...`（多臂放平，臂内完整子句）；各臂独立规划
  后由 UnionPlan 顺序流式拼接，非 ALL 全局去重；列数不一致报错；混合 UNION/UNION ALL 解析期拒绝。
- [x] **RIGHT / FULL JOIN**：三种 join 执行器早已实现 Right/Full 语义（matched 标记 + 收尾补 NULL），
  门闩在物理规划器只让 Inner/Left 走 Hash/Merge；已放开，等值 Right/Full 升级 HashJoin/MergeJoin，
  并补回归测试（此前仅能经 NLJ 回退且无测试覆盖）。
- [x] **SAVEPOINT / ROLLBACK TO / RELEASE**：事务内命名保存点，建立时深拷贝写缓冲/读集快照，
  ROLLBACK TO 恢复快照并销毁其后保存点（自身保留可重复回滚）；行锁保守保持到事务结束。
- [x] **去相关化 OPT-1（相关 EXISTS → 非相关 IN）**：`EXISTS(… S.a = outer.b AND P)` 且 P 非相关、
  纯 AND 合取、无 DISTINCT/GROUP/LIMIT 等 → 改写为 `outer.b IN (SELECT S.a WHERE P)`（预代换一次执行，
  可命中索引，避免 O(N×M) 逐行 apply）；仅限非 NOT 上下文（NOT 节点翻转标志，双重否定还原），
  NOT EXISTS 与 NOT IN 在结果含 NULL 时语义不同故不改写。
- [x] **代价决策 OPT-2/3（范围选择率 → IndexScan vs SeqScan）**：列 min/max 统计取自有序索引首尾键
  （O(1)，无新基础设施）；大表（≥128 行粗估）上覆盖率 >50% 的范围/BETWEEN 落回 SeqScan+Filter
  （超集索引逐 pk 点查比顺序流式扫描更贵）；小表/非数值/无统计维持既有行为。
- [x] **Top-N 下推（ORDER BY + LIMIT）**：执行器早有 K 元堆 Top-N 实现但规划器从未设置
  OrderByPlan.limit（死代码）；build_limit 现将 limit/offset 下推给子 OrderByPlan（K=limit+offset
  只保前 K 小，外层 Limit 负责 offset 裁剪），替代全量物化 + 全排序。
- [x] **去相关化 OPT-5（相关 IN → 非相关 IN）**：`outer.b IN (SELECT S.col WHERE S.x = outer.b AND P)`
  且相关叶的外层列恰为 IN 左表达式（等价性保证）→ 摘除相关叶改写为非相关；同 OPT-1 的守护
  （纯 AND、无 DISTINCT/GROUP/LIMIT、非 NOT 上下文），不匹配回退 nested apply。
- [x] **代价决策 OPT-6（NDV → 等值 IndexScan vs SeqScan）**：低基数列（NDV<4，如布尔/枚举）上
  等值单键预期覆盖 ≥25% 行 → 落回 SeqScan+Filter；NDV 计数用有序索引 upper_bound 跳跃且带上限
  （O(4·log n)，无新基础设施）；小表/高基数维持走索引。
- [x] **陈旧计划缓存失效 OPT-7（统计指纹）**：代价决策会被烘入缓存计划，而旧缓存只在 DDL 时清空——
  表从 10 行涨到 10 万行后计划永不重算；现缓存条目携带各表行数 log2 桶指纹，量级变化即
  失效重规划（同量级内指纹稳定，缓存仍有效）。
- [x] **CTE v1（WITH ... AS，解析期内联改写）**：`WITH name AS (SELECT * FROM t [WHERE ...]) [,...] SELECT ...`；
  body 限制 `SELECT * FROM 单表 [WHERE ...]`；内联时 CTE 名改写为基表+别名，
  body WHERE 合取并入引用处（FROM→主 WHERE， JOIN ON→并入 ON）；
  RIGHT/FULL JOIN 侧体带 WHERE 射绝（不等价）；支持多 CTE + 显式别名 + UNION 臂。
- [ ] 优化器后续：直方图、join 基数估算。
- [ ] 范围条件走索引、多列索引。
- [x] **CBO 第一步：行数统计驱动 JOIN 重排**：存储引擎 `estimate_row_count`（LSM：memtable 条目数 +
  SSTable 字节粗估，不解码）；R5 小表左置改用真实行数（Filter 1/3、Aggregate 1/10 选择率传播）。
- [x] CBO 后续：列级统计/直方图（ANALYZE + MCV/等高直方图 + 选择性估计库 + auto-ANALYZE）、代价模型驱动的算子选择（Cost{startup,total}，IndexScan/SeqScan 与 Hash/Merge/NL 按代价比较）。
- [x] **BOOLEAN / DATE / TIMESTAMP 类型 v1（存储映射）**：BOOL/BOOLEAN→Int64 存 0/1（TRUE/FALSE
  字面量解析为 1/0，表达式与 VALUES 均支持）；DATE/TIMESTAMP/DATETIME→Text ISO-8601 字符串
  （字典序即时间序，范围/BETWEEN/索引自然生效）；独立 TypeKind + 域校验（拒绝非 0/1、非合法日期）待后续。
- [ ] DECIMAL；参数化查询。
- [x] **直方图级范围选择率 OPT-8**：有序索引即精确分布（等高直方图每桶 1 条的极限形态），
  `index_range_fraction` 定界后计数、超阈即短路；替换线性 min/max 插值（倾斜分布下后者系统性
  错判，测试实锤两个方向的误判场景）；任意可比较类型含字符串；无探针时回退线性模型。
- [x] **join 基数乘积模型 OPT-9**：R5 的 Join 估算从 max(L,R) 升级为经典 |L|·|R|/max(NDV_l,NDV_r)
  （等值 ON + 任一侧 join key 有索引时；绑定名校验；NDV=1 即真·笛卡尔积）；重复键 join 膨胀
  现能被识别并把小表换到 build 侧；无法提取时回退 max。

## P4 — 高可用与复制

- [ ] WAL 日志复制（主从）、只读副本。
- [ ] 基于共识（Raft）的自动故障转移。
- [ ] 读写分离、连接池、限流增强。
