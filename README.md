<div align="right">
  <b>中文</b> | <a href="README_EN.md">English</a>
</div>

<div align="center">

# CoroDB

**C++23 协程驱动的关系型数据库**

[![Language](https://img.shields.io/badge/language-C%2B%2B23-blue)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)]()

</div>

CoroDB 是一个从零构建的关系型数据库，采用 LSM-Tree 存储引擎、Volcano 协程执行器、MVCC 事务系统。严格 OOP 设计，模块边界清晰，代码量控制在约两万行。

> 📖 **用户手册**: [USER_MANUAL.md](USER_MANUAL.md) — 安装、SQL 命令参考、架构详解

---

## 目录

- [快速上手](#快速上手)
- [核心特性](#核心特性)
- [SQL 功能演示](#sql-功能演示)
- [查询优化器](#查询优化器)
- [并发控制](#并发控制)
- [模块详解](#模块详解)
- [数据格式规范](#数据格式规范)
- [构建与运行](#构建与运行)
- [配置选项](#配置选项)
- [开发路线图](#开发路线图)
- [贡献指南](#贡献指南)
- [许可证](#许可证)

---

## 设计理念

### 为什么选择协程？

传统数据库执行器使用回调或状态机处理流水线，代码复杂难以维护。CoroDB 采用 C++23 `std::generator` 协程，将异步代码写成同步风格。每个查询算子都是一个协程，数据像流水一样在算子之间流动——惰性求值，按需产出。

### 为什么选择 LSM-Tree？

LSM-Tree 将随机写转化为顺序追加，写吞吐极高。写路径简单直观（WAL → MemTable → flush SSTable），读路径分层清晰（MemTable → L0 → L1 → …）。同时天然包含 Compaction、Tombstone GC 等数据库核心机制，非常适合作为教学引擎。

### 为什么从零构建？

成熟数据库代码量数百万行，难以学习。CoroDB 控制在约两万行 C++23，每个模块清晰可读。

---

## 核心特性

### 存储引擎
- **LSM-Tree**: MemTable（`std::map` 红黑树）+ SSTable L0-L3 + WAL，Leveled Compaction + Tombstone GC
- **Buffer Pool**: Clock 替换算法（LRU 近似），16 路分片锁降低竞争
- **Bloom Filter**: SSTable 页脚含 Bloom + key range，点查跳过无关 SSTable
- **原子写入**: `.tmp` → `fsync` → `rename`，崩溃安全；SSTable 解码 LRU 缓存
- **扫描缓存**: 全表扫描结果按写入版本号缓存，无写入时共享锁并发读取

### 查询引擎
- **Volcano 协程执行器**: C++23 `std::generator` 惰性求值，数据在算子间流水传递
- **9 种物理算子**: SeqScan / IndexScan / Filter / Project / HashJoin / MergeJoin / NestedLoopJoin / HashAggregate / SortAggregate / OrderBy / Limit
- **两段式优化器**: LogicalPlanner → 5 条重写规则定点迭代（最多 16 轮）→ PhysicalPlanner（JOIN 小表左置基于存储引擎行数统计）
- **算子选择**: 等值/范围/集合索引条件（`=`/`<`/`>`/`BETWEEN`/`IN`）与多列等值合取（`a=? AND b=?` 命中复合索引）→ IndexScan 升级；等值 JOIN → HashJoin / MergeJoin（预排序跳过重排）；GROUP BY 匹配排序 → SortAggregate 吸收 Sort
- **LRU 计划缓存**: 标准化 SQL 到物理计划的缓存（默认 128 条），DDL 自动失效

### 事务系统
- **MVCC 快照隔离**: 每行携带 `commit_ts`，查询按 `snapshot_ts` 过滤可见版本，Compaction 全层级 GC
- **四种隔离级别**: READ UNCOMMITTED / READ COMMITTED / REPEATABLE READ / SERIALIZABLE
- **Serializable 冲突检测**: 表级 SIREAD 锁 + 写计数器 + 行级读集验证，提交时检测幻读
- **行级写写冲突**: first-committer-wins 行级写意图锁，表级锁 32 路分片，全局 5s 超时
- **断连回滚**: 客户端断开自动回滚活跃事务并释放所有行锁
- **崩溃原子恢复**: WAL 全局提交日志封口每次提交，重启时仅回放已提交事务（含跨表全有或全无）

### SQL 支持
- **DDL**: CREATE/DROP TABLE, CREATE/DROP INDEX
- **DML**: INSERT/UPDATE/DELETE
- **查询**: SELECT / DISTINCT / INNER JOIN / LEFT JOIN / GROUP BY / HAVING / ORDER BY / LIMIT / OFFSET
- **聚合**: COUNT / SUM / AVG / MIN / MAX，AVG() 返回 IEEE 754 Float64
- **表达式**: 算术（`+ - * / %`）、字符串拼接（`||`）、COALESCE / NULLIF / UPPER / LOWER / SUBSTR / TRIM / LENGTH / ABS
- **诊断**: EXPLAIN / EXPLAIN ANALYZE（PostgreSQL 风格计划树 + 算子级耗时与行数）
- **预处理**: PREPARE / EXECUTE / DEALLOCATE PREPARE（会话级计划注册）
- **管理**: CHECKPOINT（强制刷盘+全层级压缩+截断 WAL）、SHOW STATUS
- **约束**: 写入时强制 NOT NULL / 类型匹配 / 主键唯一性（支持多列 `PRIMARY KEY` 标记的复合主键）

### 网络与持久性
- **Multi-Reactor 模式**: Main Reactor 接受连接 → Sub Reactor I/O 线程池（Round-Robin）处理读写 → Worker 线程池执行 SQL
- **跨平台**: epoll 边沿触发 (Linux) / WSAPoll (Windows)
- **WAL 组提交**: Leader-Follower 模式批量 fsync，可配置延迟与批次大小
- **连接管理**: 非阻塞 socket + 64MB 单连接缓冲区上限（防 OOM DoS）+ 空闲超时 + 单事件批量 accept（64）

---

## 项目结构

```
corodb/
├── include/corodb/
│   ├── ast/                  # AST 节点定义
│   ├── common/               # 类型系统、配置、日志、表格渲染
│   ├── db/                   # Database 门面、Session
│   ├── executor/             # 协程执行器、表达式/布尔求值器
│   ├── net/                  # 网络工具、平台兼容层
│   ├── optimizer/
│   │   ├── logical/          # 逻辑规划器 + 重写规则
│   │   └── physical/         # 物理规划器（算子选择）
│   ├── plan/                 # 逻辑/物理计划节点
│   ├── process/              # QueryProcessor、TransactionController、ExplainPrinter
│   ├── server/               # 服务器启动接口
│   ├── sql/                  # 词法分析 + 递归下降解析器
│   ├── storage/              # LSM 引擎、Buffer Pool、Table、Catalog
│   ├── threading/            # EventLoop、ReactorServer、Connection、ThreadPool
│   └── txn/                  # TransactionManager、LockManager、RowLockManager
├── src/                      # 实现文件（镜像 include/ 布局）
├── tests/                    # Google Test 单元测试
└── CMakeLists.txt
```

---

## 快速上手

### 构建

```bash
# Windows（VS 2026 x64 Developer PowerShell）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 启动

```bash
cd bin
# 首次启动会自动生成 corodb.conf，也可手动运行 corodb_genconfig 预先创建
./corodb_server         # 启动，监听 127.0.0.1:4000
```

```
[2026-05-23 05:59:33] [INFO] Initializing thread-safe database...
[2026-05-23 05:59:33] [INFO] Database initialized with 16 worker threads
[2026-05-23 05:59:33] [INFO] ReactorServer initialized: port=4000
[2026-05-23 05:59:33] [INFO] ReactorServer started on port 4000
```

### 连接

```bash
./csql                  # 交互模式
./csql -e "SELECT 1;"   # 单语句模式
```

---

## SQL 功能演示

以下所有输出均为 CoroDB 实际运行结果。

### DDL：表与索引

```sql
sql> CREATE TABLE employees (id INT, name TEXT, dept TEXT, salary INT);
  OK

sql> CREATE TABLE departments (id INT, name TEXT, budget INT);
  OK

sql> CREATE INDEX idx_emp_dept ON employees (dept);
  OK

sql> DROP INDEX idx_emp_dept;
  OK
```

### DML：增删改查

```sql
sql> INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 75000);
  OK
sql> INSERT INTO employees VALUES (2, 'Bob', 'Marketing', 60000);
  OK
sql> INSERT INTO employees VALUES (3, 'Carol', 'Engineering', 80000);
  OK
sql> INSERT INTO employees VALUES (4, 'Dave', 'Marketing', 65000);
  OK
sql> INSERT INTO departments VALUES (1, 'Engineering', 500000);
  OK
sql> INSERT INTO departments VALUES (2, 'Marketing', 300000);
  OK

sql> SELECT * FROM employees;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  |  3 | Carol | Engineering |  80000 |
  |  4 | Dave  | Marketing   |  65000 |
  +----+-------+-------------+--------+

sql> SELECT name, salary FROM employees WHERE dept = 'Engineering';
  +-------+--------+
  | name  | salary |
  +-------+--------+
  | Alice |  75000 |
  | Carol |  80000 |
  +-------+--------+

sql> SELECT * FROM employees WHERE salary > 65000;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  1 | Alice | Engineering |  75000 |
  |  3 | Carol | Engineering |  80000 |
  +----+-------+-------------+--------+

sql> UPDATE employees SET salary = 85000 WHERE name = 'Alice';
  OK

sql> DELETE FROM employees WHERE id = 4;
  OK
```

### 聚合与分组

```sql
sql> SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal, SUM(salary) AS total
   ... FROM employees GROUP BY dept;
  +-------------+-----+--------------+--------+
  | dept        | cnt | avg_sal      | total  |
  +-------------+-----+--------------+--------+
  | Engineering |   2 | 77500.000000 | 155000 |
  | Marketing   |   2 | 62500.000000 | 125000 |
  +-------------+-----+--------------+--------+
```

支持的聚合函数：`COUNT(*)`、`COUNT(col)`、`SUM`、`AVG`、`MIN`、`MAX`。`AVG()` 返回 `FLOAT64` 类型。

### JOIN 查询

```sql
sql> SELECT e.name, e.dept, d.budget
   ... FROM employees e INNER JOIN departments d ON e.dept = d.name;
  +-------+-------------+--------+
  | name  | dept        | budget |
  +-------+-------------+--------+
  | Alice | Engineering | 500000 |
  | Bob   | Marketing   | 300000 |
  | Carol | Engineering | 500000 |
  +-------+-------------+--------+
```

支持 `INNER JOIN` 和 `LEFT JOIN`，搭配 `ON` 条件。

### 排序与分页

```sql
sql> SELECT * FROM employees ORDER BY salary DESC;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  3 | Carol | Engineering |  80000 |
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  +----+-------+-------------+--------+

sql> SELECT * FROM employees ORDER BY dept ASC, salary DESC LIMIT 3;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  3 | Carol | Engineering |  80000 |
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  +----+-------+-------------+--------+
```

### 事务控制

```sql
sql> BEGIN;
  Transaction started with ID: 1
sql> INSERT INTO employees VALUES (5, 'Eve', 'Finance', 70000);
  OK
sql> SELECT COUNT(*) FROM employees;
  +-------+
  | count |
  +-------+
  |     5 |
  +-------+
sql> COMMIT;
  Transaction committed

sql> ROLLBACK;
ERROR: [Process] No active transaction to rollback
```

#### 隔离级别设置

```sql
sql> SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED;
  Isolation level updated
sql> SET TRANSACTION ISOLATION LEVEL READ COMMITTED;
  Isolation level updated
sql> SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
  Isolation level updated
sql> SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
  Isolation level updated
```

### EXPLAIN / EXPLAIN ANALYZE

```sql
sql> EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering' ORDER BY salary DESC;
  +-----------------------------------------------------------------------+
  | QUERY PLAN                                                            |
  +-----------------------------------------------------------------------+
  | Project [id AS id, name AS name, dept AS dept, salary AS salary]      |
  |   ->  Sort                                                            |
  |       Sort Key: salary DESC                                           |
  |     ->  Filter: (dept = 'Engineering')                                |
  |       ->  Seq Scan on employees                                       |
  +-----------------------------------------------------------------------+

sql> EXPLAIN ANALYZE SELECT dept, COUNT(*) FROM employees GROUP BY dept;
  +-----------------------------+
  | QUERY PLAN                  |
  +-----------------------------+
  | Hash Aggregate              |
  |     Group Key: dept         |
  |   ->  Seq Scan on employees |
  | Execution Time: 2.892900 ms |
  +-----------------------------+
```

### 预处理语句

```sql
sql> PREPARE eng FROM 'SELECT name, salary FROM employees WHERE dept = ''Engineering''';
  PREPARE

sql> EXECUTE eng;
  +-------+--------+
  | name  | salary |
  +-------+--------+
  | Alice |  75000 |
  | Carol |  80000 |
  +-------+--------+

sql> DEALLOCATE PREPARE eng;
  DEALLOCATE PREPARE

sql> DEALLOCATE PREPARE ALL;   -- 一次清空所有
  DEALLOCATE ALL
```

注意：字符串内的单引号需转义为 `''`。DDL 操作会自动清空所有预处理语句。

### 管理命令

```sql
sql> SHOW STATUS;
  +---------------------+-------------+
  | metric              | value       |
  +---------------------+-------------+
  | active_transactions | 0           |
  | plan_cache_entries  | 0           |
  | tables              | 2           |
  | storage_engine      | LSM-Tree    |
  | database_version    | 0.1.0       |
  +---------------------+-------------+

sql> CHECKPOINT;
  CHECKPOINT
```

`CHECKPOINT` 强制刷盘所有 MemTable → 全层级 Compaction → 截断 WAL。之后可直接拷贝 `data/` 目录做备份。

---

## 查询优化器

CoroDB 采用**两段式查询优化器**：逻辑规划 → 规则重写（固定点迭代） → 物理规划。所有重写规则以启发式方式工作，无需统计信息。

### 优化流水线

```
SQL AST ──> LogicalPlanner ──> LogicalPlan ──> RuleSet (5 rules, fixed-point) ──> PhysicalPlanner ──> PhysicalPlan
```

1. **LogicalPlanner**：将 AST 转换为逻辑计划树。SELECT 语句按 From → Join → Where → GroupBy → Having → OrderBy → Limit → Project 的顺序自底向上构建。
2. **RuleSet**：5 条重写规则按固定顺序迭代应用，直到计划不再变化或达到 16 轮上限。
3. **PhysicalPlanner**：将逻辑节点替换为具体物理算子，做关键算法选择。

### 五条重写规则

规则按固定顺序（R3→R1→R4→R2→R5）定点迭代，每轮遍历全部 5 条，直到计划不再变化（最多 16 轮）。

---

#### R1 谓词下推（Predicate Pushdown）

将 WHERE 条件推向数据源，减少上层算子处理的数据量。对 JOIN 按列所属的表分发单表谓词。

**触发条件**：Filter 节点的谓词可拆分为 AND 合取项，且合取项只引用单个子树的列。

**下推路径**：Filter→Project→Sort→Join（分发左右）→Filter（合并）→Scan。

```sql
SELECT e.name FROM employees e JOIN departments d ON e.dept_id = d.id
WHERE e.salary > 80000;
```

优化前：
```
Project [e.name]
  ->  Filter: (e.salary > 80000)
    ->  Join (INNER, e.dept_id = d.id)
      ->  Scan on employees
      ->  Scan on departments
```

优化后：`e.salary > 80000` 仅引用 employees，被推到 Join 左侧。
```
Project [e.name]
  ->  Join (INNER, e.dept_id = d.id)
    ->  Filter: (e.salary > 80000)
      ->  Scan on employees
    ->  Scan on departments
```

多表谓词分发：
```sql
SELECT * FROM orders o JOIN products p ON o.pid = p.id
WHERE o.amount > 100 AND p.price < 500;
```
优化后：`o.amount > 100` 推到左侧，`p.price < 500` 推到右侧，各自在 Scan 上方过滤。

---

#### R2 投影合并（Projection Merge）

消除冗余的 Project 节点。

**触发条件**：相邻两个 Project，外层每列都是无表名前缀的纯 `ColumnRef`，且列名在内层 Project 的输出中存在匹配。

```sql
SELECT id, name FROM (SELECT id, name, dept, salary FROM employees) t;
```

优化前：
```
Project [id, name]
  ->  Project [id, name, dept, salary]
    ->  Scan on employees
```

优化后：内层 Project 被吸收，只保留需要的列。
```
Project [id, name]
  ->  Scan on employees
```

---

#### R3 常量折叠（Constant Folding）

编译期计算所有操作数为字面量的表达式。

**支持**：算术运算（加减乘除取模）、字符串拼接。涉及 NULL 的操作短路为 NULL。除以零不折叠。

```sql
SELECT * FROM employees WHERE salary > 50000 + 30000;
```

优化前：
```
Filter: (salary > (50000 + 30000))
  ->  Scan on employees
```

优化后：`50000 + 30000` 在优化期直接算出 `80000`。
```
Filter: (salary > 80000)
  ->  Scan on employees
```

字符串拼接同理：`'elec' || 'tronics'` 折叠为 `'electronics'`。

---

#### R4 列裁剪（Column Pruning）

从根节点向下传播所需列集合，裁剪 Project 只输出上游引用的列。

**传播方向**：自顶向下。根节点 `needed = nullopt`（保留所有列），逐层收集列引用并向下传递。

```sql
SELECT name FROM employees WHERE dept = 'Eng';
```

传播过程：
```
Project [name]                        ← needed = nullopt → 保留 name
  → 收集 {name}
  ->  Filter: (dept = 'Eng')          ← needed = {name}, 合并谓词 {dept}
    → child_needed = {name, dept}
    ->  Scan on employees             ← 仅需扫描 2 列（即使表有 20 列）
```

JOIN 裁剪：
```sql
SELECT e.name, d.name FROM employees e JOIN departments d ON e.dept_id = d.id;
```
左侧只需 `name, dept_id`，右侧只需 `id, name`——多余的 salary、budget 等列根本不会从存储层读出。

---

#### R5 连接重排序（Join Reorder）

对 INNER JOIN 交换左右子树，使估算较小的子树在左侧。

**代价估算**：`Scan = 1`，`Join = max(left, right) + 1`，`Aggregate = (child + 1) / 2`，纯启发式，无需统计信息。

**触发条件**：INNER JOIN 且右子树估算大小 < 左子树。非内连接（LEFT/RIGHT/FULL）不重排。

```sql
SELECT * FROM (SELECT * FROM a JOIN b ON a.id = b.id) ab
JOIN c ON a.id = c.id;
```

优化前：左侧 `(a JOIN b)` 大小 = `max(1,1) + 1 = 2`，右侧 `c` 大小 = `1`。右 < 左，触发交换。
```
Join (INNER)
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
  ->  Scan on c             ← size=1
```

优化后：较小的 `c` 换到左侧。
```
Join (INNER)
  ->  Scan on c             ← size=1
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
```

---

### 物理算子选择

物理规划器根据重写后的逻辑计划选择具体物理算子：

**Scan 选择**：
- `WHERE col = literal` + 该列有索引 → **IndexScan**（跳过全表扫描）
- 否则 → **SeqScan + Filter**

**Join 选择**：
- 等值 JOIN + 两侧 Sort 首键匹配连接键 → **Merge Join**（`left_sorted/right_sorted`，跳过重排）
- 等值 JOIN 无排序 → **Hash Join**（构建哈希表+探测）
- 非等值 JOIN → **Nested Loop Join**

**Aggregate 选择**：
- 子节点 Sort 排序列匹配 GROUP BY → **Sort Aggregate**（吸收 Sort，O(1) 内存）
- 否则 → **Hash Aggregate**（内存哈希表）

### 执行计划解读

以 `EXPLAIN` 输出的计划树为例：

```sql
sql> EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering' ORDER BY salary DESC;
  +-----------------------------------------------------------------------+
  | QUERY PLAN                                                            |
  +-----------------------------------------------------------------------+
  | Project [id AS id, name AS name, dept AS dept, salary AS salary]      |  ← ①
  |   ->  Sort                                                            |  ← ②
  |       Sort Key: salary DESC                                           |
  |     ->  Filter: (dept = 'Engineering')                                |  ← ③
  |       ->  Seq Scan on employees                                       |  ← ④
  +-----------------------------------------------------------------------+
```

| 层 | 算子 | 含义 |
|----|------|------|
| ① | `Project [...]` | 最顶层：投影输出列。方括号内列出每个输出列及其别名（`col AS alias`）。`SELECT *` 会展开为所有列。 |
| ② | `Sort` | 排序层。`Sort Key` 列出排序键及方向（ASC/DESC）。多列排序时按列出顺序依次比较。 |
| ③ | `Filter: (...)` | 过滤层。括号内为 WHERE 条件。若存在索引且条件为等值比较，此处可能被 IndexScan 替代。 |
| ④ | `Seq Scan on t` | 扫描层。`Seq Scan` 表示全表顺序扫描。若为 `Index Scan` 则表示通过索引定位。`on` 后为表名。 |

**JOIN 执行计划示例**：

```sql
sql> EXPLAIN SELECT e.name, d.budget FROM employees e
   ... INNER JOIN departments d ON e.dept = d.name;
  +--------------------------------------+
  | QUERY PLAN                           |
  +--------------------------------------+
  | Project [e.name AS name, d.budget]   |  ← ① 投影
  |   ->  Hash Join (inner)              |  ← ② 连接算法
  |       Hash Cond: (e.dept = d.name)   |  ← ③ 连接条件
  |     ->  Seq Scan on employees        |  ← ④ 左子树（Build 侧）
  |     ->  Seq Scan on departments      |  ← ⑤ 右子树（Probe 侧）
  +--------------------------------------+
```

| 层 | 算子 | 含义 |
|----|------|------|
| ② | `Hash Join (inner)` | 使用哈希连接。`inner` 表示 INNER JOIN，`left` 表示 LEFT JOIN。 |
| ③ | `Hash Cond:` | 连接条件。Hash Join 要求等值连接。`e.dept = d.name` → 左表 dept 列与右表 name 列的哈希值匹配。 |
| ④ | 左子树 | Hash Join 中左侧为 Build 侧——先扫描构建哈希表。树形缩进表示父子关系。 |
| ⑤ | 右子树 | 右侧为 Probe 侧——逐行探测哈希表。同一缩进级别的兄弟子树表示并行输入。 |

**聚合执行计划示例**：

```sql
sql> EXPLAIN SELECT dept, COUNT(*) FROM employees GROUP BY dept;
  +-----------------------------+
  | QUERY PLAN                  |
  +-----------------------------+
  | Hash Aggregate              |  ← ①
  |     Group Key: dept         |  ← ②
  |   ->  Seq Scan on employees |  ← ③
  +-----------------------------+
```

| 层 | 算子 | 含义 |
|----|------|------|
| ① | `Hash Aggregate` | 使用哈希聚合（所有组在内存中）。若为 `Aggregate (sort)` 则表示流式排序聚合。 |
| ② | `Group Key:` | 分组键列名。多列分组时列出所有键，以逗号分隔。`Having` 行显示 HAVING 过滤条件（若存在）。 |
| ③ | 子节点 | 聚合的输入源。这里直接扫描 employees 表。 |

### IndexScan 与 MergeJoin 优化

```sql
sql> CREATE INDEX idx_emp_dept ON employees (dept);
sql> EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
  +--------------------------------------------+
  | QUERY PLAN                                 |
  +--------------------------------------------+
  | Index Scan on employees                    |  ← 物理优化器自动升级
  |   Index Cond: (dept = 'Engineering')       |
  +--------------------------------------------+
```

当存在索引且 WHERE 为等值条件时，物理优化器将 `Seq Scan + Filter` 替换为 `Index Scan`，直接通过索引定位目标行，跳过全表扫描。

当查询同时包含 JOIN 和 ORDER BY，且排序列恰好是连接键时，优化器选择 Merge Join 并跳过额外的排序操作（P3 优化）：

```sql
sql> EXPLAIN SELECT * FROM t1 INNER JOIN t2 ON t1.id = t2.id
   ... ORDER BY t1.id;
-- Merge Join (inner)  ← left_sorted/right_sorted marks skip re-sort
--   Merge Cond: (t1.id = t2.id)
--   ->  Sort → Seq Scan on t1     ← 仍排序（无索引前缀）
--   ->  Sort → Seq Scan on t2
```

### 计划缓存

物理计划缓存在 LRU 缓存中（默认 128 条目，仅 SELECT 语句）。键为标准化 SQL（空白折叠 + 统一大小写）。DDL 操作（CREATE/DROP TABLE/INDEX）自动清空缓存。通过 `SHOW STATUS` 可查看当前缓存条目数。

---

## 并发控制

### MVCC 多版本

CoroDB 实现了完整的多版本并发控制：
- **写入**：每行携带 `commit_ts`，不覆盖已有版本
- **读取**：按 `snapshot_ts` 过滤，只返回 `commit_ts <= snapshot_ts` 的最新版本
- **GC**：`commit_ts < min_active_read_ts` 的版本被 Compaction 回收
- **崩溃恢复**：启动时从 WAL 重建 MemTable，SSTable 提供持久化快照

### 四种隔离级别

| 级别 | 行为 |
|------|------|
| `READ UNCOMMITTED` | 读取最新版本，含未提交数据（`snapshot_ts = UINT64_MAX`） |
| `READ COMMITTED` (默认) | 每条语句获取新快照，防止脏读 |
| `REPEATABLE READ` | 整个事务使用同一快照，防止不可重复读 |
| `SERIALIZABLE` | REPEATABLE READ + 读集验证 + 表级 SIREAD 锁（防幻读） |

### 行级锁与冲突检测

并发写操作通过 `RowLockManager` 实现 first-committer-wins：
- 写入前在目标主键上注册行锁
- 如果锁已被其他活跃事务持有，抛出 `WriteConflictError`
- 提交成功后释放所有行锁
- 断连时自动回滚并释放锁

### 表级锁与超时机制

`LockManager` 提供表级共享/独占锁，32 路分片减少竞争：
- 所有锁路径带 5 秒超时，无永久阻塞
- DDL 获取全局独占锁
- `MultiTableLockGuard` 按字典序加锁避免死锁
- 全局锁同样受限

---

## 模块详解

### SQL 解析器

递归下降 LL(1) 解析器。先词法分析将 SQL 文本切分为 token 列表，再按语法函数逐层解析。表达式优先级（从低到高）：OR → AND → NOT → 比较 → 加减 → 乘除 → 原子。支持字符串内单引号转义（`''` → `'`）。

### 查询优化器

**两段式架构**：`LogicalPlanner` 将 AST 转换为逻辑计划树 → `RuleSet` 应用 5 条重写规则进行固定点迭代优化（最多 16 轮，直到计划稳定）→ `PhysicalPlanner` 选择具体物理算子。

**5 条重写规则**：
1. **谓词下推** — 将 WHERE 条件穿越 Project/Sort 推到 Scan 上方，对 JOIN 分发单表谓词
2. **投影合并** — 合并相邻 Project 节点，消除冗余投影
3. **常量折叠** — 编译期计算常量表达式，支持算术/字符串/NULL 传播
4. **列裁剪** — 从根向下传播所需列，裁剪 Project 输出
5. **连接重排序** — INNER JOIN 交换子树使较小侧在左

**物理算子选择**：
- **Scan**：`col = val` + 索引存在 → IndexScan；否则 SeqScan + Filter
- **Join**：等值 + 两侧已排序 → MergeJoin（跳过重排）；等值 + 未排序 → HashJoin；非等值 → NestedLoopJoin
- **Aggregate**：子节点排序匹配 GROUP BY → SortAggregate（吸收 Sort, O(1)内存）；否则 HashAggregate

### 查询执行器

Volcano 迭代器模型，全部使用 C++23 `std::generator` 协程实现。每个物理算子是一个协程，通过 `co_yield` 将 Record 向上游传递。支持查询超时：每条语句可配置 `statement_timeout_ms`，每算子 yield 前检查截止时间。

### 存储引擎

**LSM-Tree** 是当前唯一存储引擎。

写入路径：
```
SQL DML → WAL.append (带校验和) → fsync (组提交)
         → MemTable (std::map<MVCCKey, MemEntry>，红黑树)
         → 当 MemTable 超过阈值 (1MB) → flush 到 L0 SSTable
         → 后台 Compaction L0→L1→L2→L3
```

读取路径：
```
查询 → MemTable (最新数据)
     → SSTable Bloom Filter 检查 → L0 (可能重叠) → L1 → L2 → L3
     → 每层按 (pk ASC, commit_ts DESC) 归并，取各 pk 最新可见版本
```

### 缓冲池管理器

- **Clock 替换算法**：LRU 近似，每个页面有 usage_count
- **16 路分片锁**：降低并发竞争
- **FNV-1a 校验和**：页面级完整性保护
- **页面布局**：PageHeader (LSN + checksum + slot_count + free_start/end) + 双向槽目录

### 网络服务器

Reactor 模式：
- **Main Reactor**（主线程）：接受新连接
- **Sub Reactor**（I/O 线程池）：处理已连接 socket 的读写
- **Worker Pool**（工作线程池）：执行 SQL

连接管理：
- Round-Robin 分配到 I/O 线程
- 非阻塞 socket + 输入/输出缓冲区（各 64MB 上限）
- 可配置空闲超时自动断开
- 单事件批量 accept（最多 64 连接）

---

## 数据格式规范

### WAL 格式

```
WAL 文件头 (5 bytes):
+------------------+----------+
| Magic "WAL1" (4) | Ver (1)  |
+------------------+----------+

WAL 记录 (9+N bytes):
+----------+----------+----------+------------------+
| Type (1) | Len  (4) | Cksm (4) | Payload (N)      |
+----------+----------+----------+------------------+

类型: 0x01=INSERT  0x02=DELETE  0x03=BEGIN  0x04=COMMIT  0x05=ROLLBACK
      0x0B=INSERT(含commit_ts)   0x0C=DELETE(含commit_ts)
```

### SSTable 格式

```
SSTable 文件格式:
+--------+----------+----------+--------+--------+----------+
| LSM2(4)|SchBytes(4)|Schema(var)|Padding|Pages...|Footer   |
+--------+----------+----------+--------+--------+----------+

Footer:
+----------+----------+----------+----------+---------------+
| FT01 (4) | min_pk(8)| max_pk(8)|BloomLen(4)|BloomFilter(var)
+----------+----------+----------+----------+---------------+

记录格式: type(1) + commit_ts(8) + len(4) + data(var)
  type=1: INSERT (data = Row 编码)
  type=2: DELETE (data = 8B pk)
```

### Schema 编码

```
col_count(4) + for each col: name_len(2) + name(var) + type(1)
类型: 0x00=NULL  0x01=Int64  0x02=Text  0x03=Float64
```

### Row 编码

```
value_count(4) + for each value: tag(1) + data(var)
tag=0x00: NULL (无数据)
tag=0x01: int64_t (8 字节，小端)
tag=0x02: string  (len(4) + UTF-8 数据)
tag=0x03: double  (8 字节，小端)
```

### 页面布局

```
PageHeader (18 bytes):
+----------+----------+------------+------------+----------+
| LSN (8)  | Cksm (4) | SlotCnt (2)| FreeStart(2)| FreeEnd(2)|
+----------+----------+------------+------------+----------+

槽目录从 header 后向前增长，记录数据从页面末尾向后增长。
校验和: FNV-1a (32-bit)，计算时 checksum 字段置零。
```

### 索引文件格式

```
索引文件 = 多个 Chunk，每个 Chunk:
+-----------+----------+-------------------+
| SIDX (4)  | Count(4) | Entries...        |
+-----------+----------+-------------------+
Entry: value(tag+data) + 主键 pk（write_value 变长编码，支持 int64/字符串/浮点）

有序二级索引存 (列值 → 主键) 超集：写入即追加一条 Entry，读取合并所有 Chunk；
IndexScan 用 lookup_visible + 可见性重查过滤陈旧条目（MVCC 正确，支持等值与范围）。
```

---

## 构建与运行

### 环境要求

- C++23 编译器（MSVC 2026+ / GCC 16+ / Clang 20+）
- CMake 3.26+
- Google Test 1.14.0（通过 FetchContent 自动下载）

### 构建命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

### 测试

```bash
cd build && ctest -j8
```

### 性能基准

```bash
./bench_concurrency -c 50 -n 1500
```

---

## SQL 语法支持

### 已支持

| 类别 | 语法 |
|------|------|
| DDL | `CREATE TABLE name (col type, ...)`, `DROP TABLE name`, `CREATE INDEX idx ON t (col)` / `ON t (a, b, ...)`（多列为复合等值索引）, `DROP INDEX idx` |
| DML | `INSERT INTO t VALUES (v1, v2, ...)`, `UPDATE t SET col = v WHERE ...`, `DELETE FROM t WHERE ...` |
| 查询 | `SELECT [DISTINCT] cols FROM t [alias] [JOIN ... ON ...] [WHERE ...] [GROUP BY ... [HAVING ...]] [ORDER BY ... [ASC|DESC]] [LIMIT n] [OFFSET n]` |
| JOIN | `INNER JOIN`, `LEFT JOIN` |
| 聚合 | `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, `MAX` |
| 事务 | `BEGIN`, `COMMIT`, `ROLLBACK`, `SET TRANSACTION ISOLATION LEVEL {READ UNCOMMITTED \| READ COMMITTED \| REPEATABLE READ \| SERIALIZABLE}` |
| 计划 | `EXPLAIN stmt`, `EXPLAIN ANALYZE stmt` |
| 预处理 | `PREPARE name FROM 'sql'`, `EXECUTE name`, `DEALLOCATE PREPARE [name \| ALL]` |
| 管理 | `CREATE USER user 'pwd'`, `AUTH user 'pwd'`, `CHECKPOINT`, `SHOW STATUS` |
| 运算符 | `=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `NOT`, `+`, `-`, `*`, `/`, `%`, `\|\|` |
| 类型 | `INT`, `INT64`, `BIGINT`, `TEXT`, `STRING`, `VARCHAR`, `FLOAT`, `DOUBLE`, `FLOAT64` |

### 已知限制

| 特性 | 状态 |
|------|------|
| 子查询 | 支持非相关 `IN / NOT IN (SELECT ...)` 与 `EXISTS / NOT EXISTS (SELECT ...)`（WHERE 中，可嵌套；代换后可命中索引）；相关子查询不支持 |
| `IS NULL / IS NOT NULL` | 不支持 |
| `RIGHT JOIN` / `FULL JOIN` | 仅 INNER 和 LEFT |
| `SAVEPOINT` / 嵌套事务 | 不支持 |
| TLS 加密传输 | 不支持 |

---

## 配置选项

配置文件 `corodb.conf`（INI 格式）。首次启动服务器时自动生成到可执行文件同目录（`corodb_genconfig` 工具可手动预先创建，非必须）。共 9 个段、22 个配置项。

### `[server]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `port` | `4000` | TCP 监听端口 |
| `data_dir` | `./data` | 数据文件目录 |
| `io_threads` | `0` | I/O 线程数（0 = CPU 核心数） |
| `worker_threads` | `0` | 工作线程数（0 = CPU 核心数） |
| `max_connections` | `10000` | 最大并发连接数 |
| `reuse_port` | `true` | 端口复用（SO_REUSEPORT） |
| `idle_timeout_sec` | `0` | 空闲连接超时（秒，0 = 禁用） |
| `statement_timeout_ms` | `0` | 单条语句超时（毫秒，0 = 禁用） |

### `[storage]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `page_size` | `8192` | 页面大小（字节） |
| `buffer_pages` | `256` | 缓冲池容量（页数） |
| `memtable_size_bytes` | `1048576` | MemTable 刷新阈值（1 MB） |

### `[wal]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `sync_mode` | `fast` | 同步模式：`fast`（仅 flush）/ `durable`（内核 fsync） |
| `group_commit_delay_us` | `1000` | 组提交最长等待（微秒） |
| `group_commit_batch_size` | `64` | 组提交批量大小 |

### `[connection]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `max_buffer_size` | `67108864` | 单连接缓冲区上限（字节，默认 64 MB） |

### `[network]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `send_timeout_ms` | `30000` | 发送超时（毫秒） |
| `read_timeout_ms` | `30000` | 接收超时（毫秒） |

### `[lock_manager]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `timeout_ms` | `5000` | 表级锁等待超时（毫秒） |

### `[lsm]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `l0_compaction_threshold_bytes` | `65536` | L0 Compaction 触发阈值（字节） |
| `max_level` | `3` | LSM 最大层级数（1–7） |
| `sst_cache_entries` | `256` | SSTable 解码缓存容量（条目） |

### `[plan_cache]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `max_entries` | `128` | 计划缓存容量（条目） |

### `[thread_pool]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `max_queue_size` | `0` | 任务队列上限（0 = 无限制） |

### `[auth]` 段

| 键 | 默认值 | 说明 |
|----|--------|------|
| `password_salt` | `corodb_salt_v1` | 密码哈希盐值 |

---

## 性能基准

以下数据来自 Windows 11, 50 并发客户端, 每客户端 1500 请求：

| 场景 | QPS | P50 (ms) | P99 (ms) |
|------|-----|----------|----------|
| Simple SELECT | 9,483 | 4.65 | 15.25 |
| Full Table Scan (5000 rows) | 6,637 | 6.87 | 19.72 |
| Aggregate (COUNT/SUM) | 8,153 | 5.98 | 10.76 |
| JOIN | 3,640 | 7.02 | 15.53 |
| Concurrent INSERT | 14,302 | 2.12 | 4.56 |
| Concurrent UPDATE | 4,177 | 11.60 | 30.48 |
| Concurrent DELETE | 15,756 | 1.28 | 9.15 |
| Mixed 80R/20W | 1,023 | 42.34 | 158.42 |
| Transaction Throughput | 2,527 TPS | 16.61 | 95.06 |

---

## 开发路线图

### 已完成

**存储引擎** — LSM-Tree (MemTable + SSTable L0-L3 + WAL)、Buffer Pool (Clock + 16-shard)、内核级 fsync（默认 durable）、SSTable 原子写入、WAL 记录校验和、Bloom Filter + key range 页脚、增量索引写入、SSTable 解码缓存 LRU、全层级 GC、WAL 全局提交日志 + 崩溃原子恢复（含跨表事务）、CHECKPOINT 提交日志 GC、主键泛化（int64/字符串/浮点任意标量）

**查询引擎** — SQL 解析器、Volcano 协程执行器、两段式优化器（5 条重写规则）、MergeJoin 预排序优化、EXPLAIN + EXPLAIN ANALYZE、查询超时、LRU 计划缓存、Float64 类型 + AVG() 浮点、字符串转义、等值/范围 IndexScan（value→主键有序二级索引）、写入约束强制（NOT NULL/类型/主键唯一）

**事务系统** — 四种隔离级别 + MVCC + Serializable 幻读防护、行级写写冲突检测、表级锁超时、全局锁超时、客户端断连回滚

**网络与安全** — Reactor 服务器 (epoll/WSAPoll)、批量 accept、缓冲限制、空闲超时、SIGPIPE 处理、有界线程池、AUTH 密码认证 (SHA-256)、预处理语句

**运维** — 结构化日志 (ERROR/WARN/INFO/DEBUG)、CHECKPOINT 在线备份、SHOW STATUS 监控

### 计划中

- 基于代价的优化器（CBO）
- 相关子查询（非相关 `IN (SELECT ...)` / `EXISTS` 已支持）
- `SAVEPOINT` 嵌套事务
- TLS 传输加密
- WAL 压缩
- 并行查询执行

## 贡献指南

欢迎提交 Issue 和 Pull Request。

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'Add amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 创建 Pull Request

代码风格：C++23，`/** @brief */` Doxygen 注释，中文文档。

---

## 许可证

本项目采用 [MIT 许可证](LICENSE)。

---

## 致谢

- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/)
- [LevelDB](https://github.com/google/leveldb)
- [PostgreSQL](https://www.postgresql.org/)
- [SQLite](https://sqlite.org/)
- [*Database Internals* by Alex Petrov](https://www.databass.dev/)
