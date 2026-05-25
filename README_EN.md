<div align="right">
  <a href="README.md">中文</a> | <b>English</b>
</div>

<div align="center">

# CoroDB

**A coroutine-driven relational database in C++23**

[![Language](https://img.shields.io/badge/language-C%2B%2B23-blue)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)]()

</div>

CoroDB is a relational database built from scratch, featuring an LSM-Tree storage engine, Volcano coroutine executor, and MVCC transaction system. Strict OOP design, clear module boundaries, around 20,000 lines of code.

> 📖 **User Manual**: [USER_MANUAL_EN.md](USER_MANUAL_EN.md) — installation, SQL reference, architecture guide

---

## Table of Contents

- [Quick Start](#quick-start)
- [Core Features](#core-features)
- [SQL Feature Demo](#sql-feature-demo)
- [Query Optimizer](#query-optimizer)
- [Concurrency Control](#concurrency-control)
- [Module Reference](#module-reference)
- [Data Format Specification](#data-format-specification)
- [Build and Run](#build-and-run)
- [Configuration](#configuration)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Design Philosophy

### Why Coroutines?

Traditional database executors use callbacks or state machines, making code complex. CoroDB uses C++23 `std::generator` coroutines — each query operator is a coroutine, data flows between them like a pipeline with lazy evaluation.

### Why LSM-Tree?

LSM-Tree converts random writes into sequential appends for high write throughput. The write path is simple (WAL → MemTable → flush SSTable), the read path has clear layers (MemTable → L0 → L1 → …), and it naturally includes core mechanisms like Compaction and Tombstone GC.

### Why Build from Scratch?

Mature databases span millions of lines. CoroDB stays around 20,000 lines of C++23 — each module is clearly readable.

---

## Core Features

| Area | Feature | Description |
|------|---------|-------------|
| **Storage** | LSM-Tree Engine | MemTable (red-black tree) + SSTable L0-L3 + WAL, Leveled Compaction + Tombstone GC |
| | Buffer Pool | Clock replacement, 16-shard locking, FNV-1a page checksums |
| | Atomic Writes | SSTable via `.tmp` → fsync → atomic rename, crash-safe |
| | Bloom Filter | SSTable V2 footer with bloom + key range; skip irrelevant SSTables |
| | Incremental Indexes | Append-based chunks + dedup on read + periodic compaction |
| **Durability** | Kernel fsync | `::fsync` / `::_commit` after every WAL write |
| | Group Commit | Leader-follower batch fsync, configurable delay/batch size |
| | Checksums | FNV-1a per page, verified on every read |
| **MVCC** | Snapshot Isolation | Per-row commit_ts, snapshot_ts filtering |
| | commit_ts Retention | Retained until no active snapshot references it, then GC prunes |
| | All-level GC | GC runs at every compaction level |
| **Transactions** | 4 Isolation Levels | READ UNCOMMITTED / READ COMMITTED / REPEATABLE READ / SERIALIZABLE |
| | Serializable Phantoms | Table-level SIREAD locks + write-counter detection |
| | Row-level Conflicts | First-committer-wins, auto-detect and abort |
| | Lock Timeouts | All lock paths have 5-second timeout, no indefinite blocking |
| | Disconnect Rollback | Auto-rollback active txns on client disconnect |
| **Query** | Volcano Executor | C++23 `std::generator` coroutines, lazy evaluation |
| | Operators | SeqScan / IndexScan / Filter / Project / HashJoin / MergeJoin / NestedLoopJoin / HashAggregate / SortAggregate / OrderBy / Limit |
| | Float64 Type | IEEE 754 double, AVG() returns float, implicit int↔float promotion |
| | Query Timeout | Per-statement timeout, checked at each co_yield |
| **Optimizer** | Two-Phase | LogicalPlanner → RuleSet → PhysicalPlanner |
| | Rewrite Rules | Predicate pushdown / column pruning / JoinReorder / IndexScan upgrade / LimitPushdown |
| | Plan Cache | LRU cache (normalized SQL → physical plan), DDL auto-invalidates |
| | MergeJoin Opt | Skip re-sort when inputs already ordered |
| **SQL** | DDL | CREATE/DROP TABLE, CREATE/DROP INDEX |
| | DML | INSERT/UPDATE/DELETE |
| | Query | SELECT/JOIN/GROUP BY/HAVING/ORDER BY/LIMIT/OFFSET |
| | EXPLAIN | PostgreSQL-style plan tree |
| | EXPLAIN ANALYZE | Plan + per-operator row count & timing |
| | PREPARE/EXECUTE | Prepared statements + session-level registry |
| | Admin | CHECKPOINT / SHOW STATUS |
| **Networking** | Reactor Pattern | Main Reactor + Sub Reactor I/O pool + Worker pool |
| | Cross-platform | epoll (Linux) / WSAPoll (Windows) |
| | Buffer Limits | 64 MB input/output caps, OOM DoS prevention |
| | Idle Timeout | Configurable auto-disconnect |
| | Batch Accept | Up to 64 connections per event |
| **Security** | Password Auth | AUTH command + SHA-256 hashing + UserManager |
| **Operations** | Structured Logging | ERROR/WARN/INFO/DEBUG + timestamps + file output |
| | CHECKPOINT | Force flush + compact + truncate WALs, online backup |

---

## Project Structure

```
corodb/
├── include/corodb/
│   ├── ast/                  # AST node definitions
│   ├── common/               # Types, config, logger, table renderer
│   ├── db/                   # Database facade, Session
│   ├── executor/             # Coroutine executor, expression/bool evaluators
│   ├── net/                  # Network utilities, platform compatibility
│   ├── optimizer/
│   │   ├── logical/          # Logical planner + rewrite rules
│   │   └── physical/         # Physical planner (operator selection)
│   ├── plan/                 # Logical/physical plan nodes
│   ├── process/              # QueryProcessor, TransactionController, ExplainPrinter
│   ├── server/               # Server startup interface
│   ├── sql/                  # Lexer + recursive-descent parser
│   ├── storage/              # LSM engine, Buffer Pool, Table, Catalog
│   ├── threading/            # EventLoop, ReactorServer, Connection, ThreadPool
│   └── txn/                  # TransactionManager, LockManager, RowLockManager
├── src/                      # Implementation files (mirrors include/)
├── tests/                    # Google Test unit tests
└── CMakeLists.txt
```

---

## Quick Start

### Build

```bash
# Windows (VS 2026 x64 Developer PowerShell)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run

```bash
cd bin
# Config auto-generated on first startup; optionally run corodb_genconfig to pre-create it
./corodb_server         # Start on 127.0.0.1:4000
```

```
[2026-05-23 05:59:33] [INFO] Initializing thread-safe database...
[2026-05-23 05:59:33] [INFO] Database initialized with 16 worker threads
[2026-05-23 05:59:33] [INFO] ReactorServer initialized: port=4000
[2026-05-23 05:59:33] [INFO] ReactorServer started on port 4000
```

### Connect

```bash
./csql                  # Interactive
./csql -e "SELECT 1;"   # Single statement
```

---

## SQL Feature Demo

All output below is from an actual running CoroDB instance.

### DDL: Tables & Indexes

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

### DML: Insert, Query, Update, Delete

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

### Aggregation & GROUP BY

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

Supported aggregates: `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, `MAX`. `AVG()` returns `FLOAT64`.

### JOIN

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

Supports `INNER JOIN` and `LEFT JOIN` with `ON` conditions.

### ORDER BY, LIMIT, OFFSET

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

### Transactions

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

### Prepared Statements

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
sql> DEALLOCATE PREPARE ALL;
  DEALLOCATE ALL
```

Note: `''` inside a string literal escapes a single `'`. DDL invalidates all prepared statements.

### Administration

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

---

## Query Optimizer

CoroDB uses a **two-phase query optimizer**: logical planning → rule-based rewrite (fixed-point iteration) → physical planning. All rewrite rules are heuristic — no statistics required.

### Pipeline

```
SQL AST ──> LogicalPlanner ──> LogicalPlan ──> RuleSet (5 rules, fixed-point) ──> PhysicalPlanner ──> PhysicalPlan
```

1. **LogicalPlanner**: Converts AST to logical plan tree. SELECT statements build bottom-up: From → Join → Where → GroupBy → Having → OrderBy → Limit → Project.
2. **RuleSet**: 5 rewrite rules applied in fixed order, iterating until the plan stabilizes or 16 rounds elapse.
3. **PhysicalPlanner**: Replaces logical nodes with concrete physical operators, making key algorithm choices.

### Five Rewrite Rules

Rules are applied in fixed order (R3→R1→R4→R2→R5) using fixed-point iteration until the plan stabilizes (max 16 rounds).

---

#### R1 Predicate Pushdown

Pushes WHERE conditions toward data sources. For JOINs, predicates are distributed to the side referencing the column.

**Trigger**: Filter with AND-conjoined predicates, each referencing only one subtree's columns.

**Path**: Filter→Project→Sort→Join (distribute)→Filter (merge)→Scan.

```sql
SELECT e.name FROM employees e JOIN departments d ON e.dept_id = d.id
WHERE e.salary > 80000;
```

Before:
```
Project [e.name]
  ->  Filter: (e.salary > 80000)
    ->  Join (INNER, e.dept_id = d.id)
      ->  Scan on employees
      ->  Scan on departments
```

After: `e.salary > 80000` references only `employees`, pushed to left side.
```
Project [e.name]
  ->  Join (INNER, e.dept_id = d.id)
    ->  Filter: (e.salary > 80000)
      ->  Scan on employees
    ->  Scan on departments
```

Multi-table predicate distribution:
```sql
SELECT * FROM orders o JOIN products p ON o.pid = p.id
WHERE o.amount > 100 AND p.price < 500;
```
After: `o.amount > 100` pushed left, `p.price < 500` pushed right, each filtering above its Scan.

---

#### R2 Projection Merge

Eliminates redundant Project nodes.

**Trigger**: Two adjacent Projects; outer columns are all pure `ColumnRef` without table prefix, and names match inner Project's outputs.

```sql
SELECT id, name FROM (SELECT id, name, dept, salary FROM employees) t;
```

Before:
```
Project [id, name]
  ->  Project [id, name, dept, salary]
    ->  Scan on employees
```

After: inner Project absorbed, only needed columns remain.
```
Project [id, name]
  ->  Scan on employees
```

---

#### R3 Constant Folding

Evaluates expressions where all operands are literals at compile time.

**Supports**: arithmetic (+, -, *, /, %), string concatenation (||). NULL operands short-circuit to NULL. Division by zero is not folded.

```sql
SELECT * FROM employees WHERE salary > 50000 + 30000;
```

Before:
```
Filter: (salary > (50000 + 30000))
  ->  Scan on employees
```

After: `50000 + 30000` computed to `80000` during optimization.
```
Filter: (salary > 80000)
  ->  Scan on employees
```

String concat: `'elec' || 'tronics'` folds to `'electronics'`.

---

#### R4 Column Pruning

Propagates required column sets top-down from root. Projects output only columns actually needed upstream.

**Direction**: root → leaves. Root starts with `needed = nullopt` (keep all); each node collects column refs and passes them down.

```sql
SELECT name FROM employees WHERE dept = 'Eng';
```

Propagation:
```
Project [name]                        ← needed = nullopt → keep name
  → collects {name}
  ->  Filter: (dept = 'Eng')          ← needed = {name}, merge predicate {dept}
    → child_needed = {name, dept}
    ->  Scan on employees             ← only 2 columns read (even if table has 20)
```

JOIN pruning:
```sql
SELECT e.name, d.name FROM employees e JOIN departments d ON e.dept_id = d.id;
```
Left side only reads `name, dept_id`; right side only `id, name`. Extra columns (salary, budget, etc.) are never fetched from storage.

---

#### R5 Join Reorder

Swaps INNER JOIN subtrees so the smaller estimated side is on the left.

**Cost estimate**: `Scan = 1`, `Join = max(left, right) + 1`, `Aggregate = (child + 1) / 2`. Pure heuristic — no statistics needed.

**Trigger**: INNER JOIN with right subtree estimate < left subtree. Non-inner joins are unchanged.

```sql
SELECT * FROM (SELECT * FROM a JOIN b ON a.id = b.id) ab
JOIN c ON a.id = c.id;
```

Before: left `(a JOIN b)` size = `max(1,1) + 1 = 2`, right `c` size = `1`. Right < left → swap.
```
Join (INNER)
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
  ->  Scan on c             ← size=1
```

After: smaller `c` moved to left.
```
Join (INNER)
  ->  Scan on c             ← size=1
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
```

---

### Physical Operator Selection

The physical planner makes these key decisions:

**Scan**: `WHERE col = literal` + index → **IndexScan**; else **SeqScan + Filter**

**Join**: Equi-join + both sides sorted with matching key → **Merge Join** (skip re-sort); equi-join unsorted → **Hash Join**; non-equi → **Nested Loop Join**

**Aggregate**: Child Sort matches GROUP BY → **Sort Aggregate** (absorbs Sort, O(1) memory); else **Hash Aggregate**

### Reading Execution Plans

**Basic scan + filter + sort**:

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

| Layer | Operator | Meaning |
|-------|----------|---------|
| ① | `Project [...]` | Top layer: output column projection. `SELECT *` expands to all columns. |
| ② | `Sort` | Ordering. `Sort Key` lists sort columns and direction. |
| ③ | `Filter: (...)` | WHERE clause evaluation. May be replaced by IndexScan if an index exists. |
| ④ | `Seq Scan on t` | Full table scan. `Index Scan` means index-based lookup instead. |

**JOIN plan**:

```sql
sql> EXPLAIN SELECT e.name, d.budget FROM employees e
   ... INNER JOIN departments d ON e.dept = d.name;
  +--------------------------------------+
  | QUERY PLAN                           |
  +--------------------------------------+
  | Project [e.name AS name, d.budget]   |  ← ① Projection
  |   ->  Hash Join (inner)              |  ← ② Join algorithm
  |       Hash Cond: (e.dept = d.name)   |  ← ③ Join condition
  |     ->  Seq Scan on employees        |  ← ④ Left subtree (Build side)
  |     ->  Seq Scan on departments      |  ← ⑤ Right subtree (Probe side)
  +--------------------------------------+
```

| Layer | Operator | Meaning |
|-------|----------|---------|
| ② | `Hash Join (inner)` | Hash-based join. `(inner)` = INNER JOIN; `(left)` = LEFT JOIN. |
| ③ | `Hash Cond:` | Equi-join condition. Left table's `dept` hashed and matched against right table's `name`. |
| ④⑤ | Subtrees | Build side (left) scanned first to construct hash table. Probe side (right) then streamed. |

**Aggregation plan**:

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

| Layer | Operator | Meaning |
|-------|----------|---------|
| ① | `Hash Aggregate` | All groups in memory. `Aggregate (sort)` = streaming sort-based aggregation. |
| ② | `Group Key:` | Grouping column(s). `Having:` line appears if a HAVING clause is present. |
| ③ | Child | Input source for aggregation. |

### IndexScan and MergeJoin Optimization

```sql
sql> CREATE INDEX idx_emp_dept ON employees (dept);
sql> EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
  +--------------------------------------------+
  | QUERY PLAN                                 |
  +--------------------------------------------+
  | Index Scan on employees                    |  ← Auto-upgraded by physical planner
  |   Index Cond: (dept = 'Engineering')       |
  +--------------------------------------------+
```

When an index exists and the WHERE clause is an equality condition, the physical planner replaces `Seq Scan + Filter` with `Index Scan`, using the index to locate rows directly.

For JOIN + ORDER BY where the sort key matches the join key, the optimizer chooses Merge Join and skips the extra sort:

```sql
sql> EXPLAIN SELECT * FROM t1 INNER JOIN t2 ON t1.id = t2.id ORDER BY t1.id;
-- Merge Join (inner)  ← left_sorted/right_sorted avoids re-sort
--   Merge Cond: (t1.id = t2.id)
```

### Plan Cache

Physical plans are cached in an LRU map (max 128 entries, SELECT only). Cache key = normalized SQL (whitespace-collapsed, uppercased). DDL invalidates the entire cache. Check `SHOW STATUS` for cache size.

---

## Concurrency Control

### MVCC

Full multi-version concurrency control:
- **Write**: Each row carries a `commit_ts`, never overwriting existing versions
- **Read**: Filtered by `snapshot_ts` — only versions with `commit_ts <= snapshot_ts` are visible
- **GC**: Versions with `commit_ts < min_active_read_ts` are reclaimed during Compaction
- **Crash Recovery**: MemTable rebuilt from WAL on startup; SSTables provide durable snapshots

### Isolation Levels

| Level | Behavior |
|-------|----------|
| `READ UNCOMMITTED` | Reads latest version including uncommitted |
| `READ COMMITTED` (default) | Per-statement snapshot; no dirty reads |
| `REPEATABLE READ` | Per-transaction snapshot; no non-repeatable reads |
| `SERIALIZABLE` | REPEATABLE READ + read-set validation + table-level SIREAD locks (phantom prevention) |

### Row-Level Locking

- Writers register row locks on target primary keys
- If lock held by another active transaction → `WriteConflictError`
- Locks released on commit
- Auto-rollback on disconnect

### Table-Level Locking

32-shard `LockManager` with shared/exclusive locks:
- All lock paths have 5-second timeout — no indefinite blocking
- DDL acquires global exclusive lock
- `MultiTableLockGuard` acquires in dictionary order to prevent deadlocks

---

## Module Reference

### SQL Parser

Recursive-descent LL(1). Tokenizer → token list → per-grammar parsing functions. Expression precedence (low to high): OR → AND → NOT → Comparison → Add/Sub → Mul/Div → Atom. Supports escaped single quotes (`''` → `'`).

### Query Optimizer

**Two-phase**: `LogicalPlanner` AST→logical tree → `RuleSet` 5-rule fixed-point (max 16 iterations) → `PhysicalPlanner` selects operators.

**5 rewrite rules**: (1) Predicate pushdown — push WHERE conditions through Project/Sort toward Scan; (2) Projection merge — merge adjacent Projects; (3) Constant folding — compile-time evaluation with NULL propagation; (4) Column pruning — top-down required-column propagation; (5) Join reorder — swap INNER JOIN subtrees, smaller side left.

**Operator selection**: Scan: `col=val` + index → IndexScan; else SeqScan+Filter. Join: equi+sorted → MergeJoin (skip resort); equi → HashJoin; non-equi → NestedLoopJoin. Aggregate: child sort matches GROUP BY → SortAggregate (absorbs Sort, O(1) memory); else HashAggregate.

### Query Executor

Volcano iterator model using C++23 `std::generator` coroutines. Each operator is a coroutine; `co_yield` passes records upstream. Query timeout: configurable per-statement, checked before each `co_yield`.

### Storage Engine

**LSM-Tree** is the only engine.

Write path:
```
SQL DML → WAL.append (with checksum) → fsync (group commit)
         → MemTable (std::map, red-black tree)
         → when full (1 MB) → flush to L0 SSTable
         → background Compaction L0→L1→L2→L3
```

Read path:
```
Query → MemTable (newest)
       → Bloom filter check → L0 (may overlap) → L1 → L2 → L3
       → merge by (pk ASC, commit_ts DESC), pick newest visible per pk
```

### Buffer Pool

- Clock replacement (LRU approximation)
- 16-shard locking for concurrency
- FNV-1a page checksums for integrity
- Slotted page layout (bidirectional growth)

### Network Server

Reactor pattern:
- **Main Reactor**: accept new connections
- **Sub Reactors** (I/O thread pool): socket read/write
- **Worker Pool**: SQL execution

Connection management:
- Round-robin I/O thread assignment
- Non-blocking sockets + bounded buffers (64 MB each)
- Configurable idle timeout
- Batch accept (up to 64 per event)

---

## Data Format Specification

### WAL

```
Header (5 bytes): Magic "WAL1"(4) + Version(1)

Record (9+N bytes): Type(1) + Len(4) + Checksum(4) + Payload(N)

Types: 0x01=INSERT  0x02=DELETE  0x03=BEGIN  0x04=COMMIT  0x05=ROLLBACK
       0x0B=INSERT(commit_ts)  0x0C=DELETE(commit_ts)
```

### SSTable

```
V2 File: LSM2(4) + SchemaBytes(4) + Schema(var) + Padding + Pages... + Footer

Footer: FT01(4) + min_pk(8) + max_pk(8) + BloomLen(4) + BloomFilter(var)

Record: type(1) + commit_ts(8) + len(4) + data(var)
  type=1: INSERT (Row encoding)
  type=2: DELETE (8-byte pk)
```

### Schema Encoding

```
col_count(4) + for each: name_len(2) + name(var) + type(1)
Types: 0x00=NULL  0x01=Int64  0x02=Text  0x03=Float64
```

### Row Encoding

```
value_count(4) + for each: tag(1) + data(var)
tag=0x00: NULL (no data)
tag=0x01: int64_t (8 bytes, little-endian)
tag=0x02: string  (len(4) + UTF-8 data)
tag=0x03: double  (8 bytes, little-endian)
```

### Page Layout

```
PageHeader (18 bytes): LSN(8) + Checksum(4) + SlotCount(2) + FreeStart(2) + FreeEnd(2)

Slots grow forward from header; records grow backward from end.
Checksum: FNV-1a 32-bit, computed with checksum field zeroed.
```

### Index Format

```
Index file = multiple chunks, each:
  SIDX(4) + Count(4) + Entries...
Entry: value(tag+data) + rowid(8)

On read, merge all chunks (last entry per value wins).
Periodic compact_index_file() deduplicates.
```

---

## Build and Run

### Requirements

- C++23 compiler (MSVC 2026+ / GCC 16+ / Clang 20+)
- CMake 3.26+
- Google Test 1.14.0 (auto-downloaded via FetchContent)

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

### Test

```bash
cd build && ctest -j8
```

### Benchmark

```bash
./bench_concurrency -c 50 -n 1500
```

---

## SQL Syntax Support

### Supported

| Category | Syntax |
|----------|--------|
| DDL | `CREATE TABLE name (col type, ...)`, `DROP TABLE name`, `CREATE INDEX idx ON t (col)`, `DROP INDEX idx` |
| DML | `INSERT INTO t VALUES (v1, v2, ...)`, `UPDATE t SET col = v WHERE ...`, `DELETE FROM t WHERE ...` |
| Query | `SELECT [DISTINCT] cols FROM t [alias] [JOIN ... ON ...] [WHERE ...] [GROUP BY ... [HAVING ...]] [ORDER BY ... [ASC\|DESC]] [LIMIT n] [OFFSET n]` |
| JOIN | `INNER JOIN`, `LEFT JOIN` |
| Aggregate | `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, `MAX` |
| Transaction | `BEGIN`, `COMMIT`, `ROLLBACK`, `SET TRANSACTION ISOLATION LEVEL` |
| Plan | `EXPLAIN stmt`, `EXPLAIN ANALYZE stmt` |
| Prepared | `PREPARE name FROM 'sql'`, `EXECUTE name`, `DEALLOCATE PREPARE [name \| ALL]` |
| Admin | `CREATE USER user 'pwd'`, `AUTH user 'pwd'`, `CHECKPOINT`, `SHOW STATUS` |
| Operators | `=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `NOT`, `+`, `-`, `*`, `/`, `%`, `\|\|` |
| Types | `INT`, `INT64`, `BIGINT`, `TEXT`, `STRING`, `VARCHAR`, `FLOAT`, `DOUBLE`, `FLOAT64` |

### Known Limitations

| Feature | Status |
|---------|--------|
| Subqueries (`IN (SELECT ...)`) | Not supported |
| Range IndexScan (`col > val`) | Equality-only |
| `IS NULL / IS NOT NULL` | Not supported |
| `RIGHT JOIN` / `FULL JOIN` | INNER and LEFT only |
| `SAVEPOINT` / nested txn | Not supported |
| TLS encryption | Not supported |

---

## Configuration

File: `corodb.conf` (INI format). Auto-generated on first server startup (`corodb_genconfig` is available for manual pre-creation, but optional). 9 sections, 22 keys.

### `[server]`

| Key | Default | Description |
|-----|---------|-------------|
| `port` | `4000` | TCP listen port |
| `data_dir` | `./data` | Data directory |
| `io_threads` | `0` | I/O threads (0 = auto) |
| `worker_threads` | `0` | Worker threads (0 = auto) |
| `max_connections` | `10000` | Max connections |
| `reuse_port` | `true` | SO_REUSEPORT |
| `idle_timeout_sec` | `0` | Idle timeout (seconds, 0 = disabled) |
| `statement_timeout_ms` | `0` | Per-statement timeout (ms, 0 = disabled) |

### `[storage]`

| Key | Default | Description |
|-----|---------|-------------|
| `page_size` | `8192` | Page size (bytes) |
| `buffer_pages` | `256` | Buffer pool capacity (pages) |
| `memtable_size_bytes` | `1048576` | MemTable flush threshold (1 MB) |

### `[wal]`

| Key | Default | Description |
|-----|---------|-------------|
| `sync_mode` | `fast` | Sync mode: `fast` (flush only) / `durable` (kernel fsync) |
| `group_commit_delay_us` | `1000` | Group commit max wait (µs) |
| `group_commit_batch_size` | `64` | Group commit batch size |

### `[connection]`

| Key | Default | Description |
|-----|---------|-------------|
| `max_buffer_size` | `67108864` | Per-connection buffer limit (bytes, 64 MB) |

### `[network]`

| Key | Default | Description |
|-----|---------|-------------|
| `send_timeout_ms` | `30000` | Send timeout (ms) |
| `read_timeout_ms` | `30000` | Read timeout (ms) |

### `[lock_manager]`

| Key | Default | Description |
|-----|---------|-------------|
| `timeout_ms` | `5000` | Table lock timeout (ms) |

### `[lsm]`

| Key | Default | Description |
|-----|---------|-------------|
| `l0_compaction_threshold_bytes` | `65536` | L0 compaction trigger (bytes) |
| `max_level` | `3` | Max LSM levels (1–7) |
| `sst_cache_entries` | `256` | Decoded SSTable cache size |

### `[plan_cache]`

| Key | Default | Description |
|-----|---------|-------------|
| `max_entries` | `128` | Plan cache capacity |

### `[thread_pool]`

| Key | Default | Description |
|-----|---------|-------------|
| `max_queue_size` | `0` | Task queue limit (0 = unlimited) |

### `[auth]`

| Key | Default | Description |
|-----|---------|-------------|
| `password_salt` | `corodb_salt_v1` | Password hash salt |

---

## Benchmarks

Windows 11, 50 concurrent clients, 1500 requests each:

| Scenario | QPS | P50 (ms) | P99 (ms) |
|----------|-----|----------|----------|
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

## Roadmap

### Completed

**Storage** — LSM-Tree (MemTable + SSTable L0-L3 + WAL), Buffer Pool (Clock + 16-shard), kernel fsync, atomic SSTable writes, page checksums, Bloom filter + key range footer, incremental indexes, decode cache LRU, all-level GC

**Query** — SQL parser, Volcano coroutine executor, two-phase optimizer (5 rewrite rules), MergeJoin presorted opt, EXPLAIN + EXPLAIN ANALYZE, query timeout, plan cache, Float64 + AVG(), string escape handling

**Transactions** — 4 isolation levels + MVCC + Serializable phantom prevention, row-level conflict detection, table/global lock timeouts, disconnect rollback

**Networking & Security** — Reactor server (epoll/WSAPoll), batch accept, buffer limits, idle timeout, SIGPIPE handling, bounded thread pool, AUTH (SHA-256), prepared statements

**Operations** — Structured logging (ERROR/WARN/INFO/DEBUG), CHECKPOINT, SHOW STATUS

### Planned

- Cost-based optimizer (CBO)
- Range IndexScan
- Subqueries (`IN / EXISTS`)
- `SAVEPOINT` nested transactions
- TLS encryption
- WAL compression
- Parallel query execution

---

## Contributing

Issues and pull requests are welcome.

1. Fork the repo
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

Code style: C++23, `/** @brief */` Doxygen comments.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

## Acknowledgments

- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/)
- [LevelDB](https://github.com/google/leveldb)
- [PostgreSQL](https://www.postgresql.org/)
- [SQLite](https://sqlite.org/)
- [*Database Internals* by Alex Petrov](https://www.databass.dev/)
