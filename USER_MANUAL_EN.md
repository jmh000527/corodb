# CoroDB User Manual

> **Version**: 0.1.0 | **Project**: [github.com/corodb/corodb](https://github.com/corodb/corodb) | **License**: MIT

## Table of Contents

1. [Installation & Build](#1-installation--build)
2. [Getting Started](#2-getting-started)
3. [SQL Command Reference](#3-sql-command-reference)
   - [3.1 CREATE TABLE](#31-create-table)
   - [3.2 DROP TABLE](#32-drop-table)
   - [3.3 CREATE INDEX](#33-create-index)
   - [3.4 DROP INDEX](#34-drop-index)
   - [3.5 INSERT](#35-insert)
   - [3.6 SELECT](#36-select)
   - [3.7 UPDATE](#37-update)
   - [3.8 DELETE](#38-delete)
   - [3.9 JOIN](#39-join)
   - [3.10 GROUP BY & Aggregation](#310-group-by--aggregation)
   - [3.11 ORDER BY / LIMIT / OFFSET](#311-order-by--limit--offset)
   - [3.12 Transaction Control](#312-transaction-control)
   - [3.13 EXPLAIN](#313-explain)
   - [3.14 EXPLAIN ANALYZE](#314-explain-analyze)
   - [3.15 Prepared Statements](#315-prepared-statements)
   - [3.16 AUTH Authentication](#316-auth-authentication)
   - [3.17 CHECKPOINT](#317-checkpoint)
   - [3.18 SHOW STATUS](#318-show-status)
4. [Data Types](#4-data-types)
5. [Expressions & Operators](#5-expressions--operators)
6. [Client (csql)](#6-client-csql)
7. [Configuration Reference](#7-configuration-reference)
8. [Query Optimizer & Execution Plans](#8-query-optimizer--execution-plans)
   - [8.1 Pipeline](#81-pipeline)
   - [8.2 Logical Planning](#82-logical-planning)
   - [8.3 Five Rewrite Rules](#83-five-rewrite-rules)
   - [8.4 Physical Operator Selection](#84-physical-operator-selection)
   - [8.5 Reading Execution Plans](#85-reading-execution-plans)
   - [8.6 Plan Examples](#86-plan-examples)
9. [Architecture Overview](#9-architecture-overview)
10. [Known Limitations](#10-known-limitations)

---

## 1. Installation & Build

### Requirements

| Platform | Compiler | Tools |
|----------|----------|-------|
| Windows | MSVC 2026+ (C++23) | CMake 3.26+ |
| Linux | GCC 16+ / Clang 20+ | CMake 3.26+ |
| macOS | Clang 20+ | CMake 3.26+ |

Google Test 1.14.0 is auto-downloaded via CMake `FetchContent` — no manual installation needed.

### Build

```bash
# 1. Clone
git clone <repo-url> && cd corodb

# 2. Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build --config Release -j

# 4. Output
#   bin/corodb_server      — database server
#   bin/csql               — CLI client
#   bin/corodb_genconfig   — config file generator
#   bin/bench_concurrency  — concurrency benchmark
```

### Run Tests

```bash
cd build && ctest -j8 --output-on-failure
```

---

## 2. Getting Started

### Generate Default Config

```bash
cd bin
# Config auto-generated on first startup; optionally pre-create it manually:
# ./corodb_genconfig
```

### Start Server

```bash
./corodb_server
```

```
[2026-05-23 05:59:33] [INFO] Initializing thread-safe database...
[2026-05-23 05:59:33] [INFO] Database initialized with 16 worker threads
[2026-05-23 05:59:33] [INFO] ReactorServer initialized: port=4000
[2026-05-23 05:59:33] [INFO] ReactorServer started on port 4000
```

### Connect

```bash
# Interactive mode
./csql

# Single-statement mode
./csql -e "SELECT 1;"

# Pipe mode
echo "CREATE TABLE t (id INT);" | ./csql
```

Connection prompt:
```
Connected to 127.0.0.1:4000 (type exit/quit to leave)
sql>
```

### First Query

```sql
sql> CREATE TABLE hello (id INT, msg TEXT);
  OK
sql> INSERT INTO hello VALUES (1, 'Hello, CoroDB!');
  OK
sql> SELECT * FROM hello;
  +----+-----------------+
  | id | msg             |
  +----+-----------------+
  |  1 | Hello, CoroDB!  |
  +----+-----------------+
```

---

## 3. SQL Command Reference

### 3.1 CREATE TABLE

**Syntax**
```sql
CREATE TABLE table_name (
    column_name type_keyword,
    column_name type_keyword,
    ...
);
```

**Supported Type Keywords**

| Keyword | Storage | Notes |
|---------|---------|-------|
| `INT`, `INT64`, `BIGINT` | 64-bit signed int | Range [-2^63, 2^63-1] |
| `FLOAT`, `DOUBLE`, `FLOAT64` | IEEE 754 double | ~15 significant digits |
| `TEXT`, `STRING`, `VARCHAR` | UTF-8 string | Variable length |

**Example**
```sql
sql> CREATE TABLE employees (id INT, name TEXT, dept TEXT, salary INT);
  OK

sql> CREATE TABLE products (
   ...   product_id BIGINT,
   ...   name VARCHAR,
   ...   price FLOAT64,
   ...   description STRING
   ... );
  OK
```

### 3.2 DROP TABLE

**Syntax**
```sql
DROP TABLE table_name;
```

Deletes the table and all associated files (WAL, SSTable levels, indexes).

**Example**
```sql
sql> DROP TABLE products;
  OK
```

### 3.3 CREATE INDEX

**Syntax**
```sql
CREATE INDEX index_name ON table_name (column_name);
```

Creates a secondary index on the specified column. The physical planner automatically upgrades SeqScan to IndexScan for equality conditions (`col = value`).

**Example**
```sql
sql> CREATE INDEX idx_emp_dept ON employees (dept);
  OK
```

### 3.4 DROP INDEX

**Syntax**
```sql
DROP INDEX index_name;
```

**Example**
```sql
sql> DROP INDEX idx_emp_dept;
  OK
```

### 3.5 INSERT

**Syntax**
```sql
INSERT INTO table_name VALUES (value1, value2, ...);
```

Values must match the column order and count defined at table creation.

**Example**
```sql
sql> INSERT INTO employees VALUES (1, 'Alice', 'Engineering', 75000);
  OK
sql> INSERT INTO employees VALUES (2, 'Bob', 'Marketing', 60000);
  OK
sql> INSERT INTO employees VALUES (3, 'Carol', 'Engineering', 80000);
  OK
sql> INSERT INTO employees VALUES (4, 'Dave', 'Marketing', 65000);
  OK
```

### 3.6 SELECT

**Full Syntax**
```sql
SELECT [DISTINCT] column_list | *
FROM table_name [alias]
[ JOIN ... ON ... ]
[ WHERE condition ]
[ GROUP BY column_list [ HAVING condition ] ]
[ ORDER BY column [ASC|DESC] [, ...] ]
[ LIMIT row_count ]
[ OFFSET row_count ]
```

#### 3.6.1 Basic Query

```sql
sql> SELECT * FROM employees;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  |  3 | Carol | Engineering |  80000 |
  |  4 | Dave  | Marketing   |  65000 |
  +----+-------+-------------+--------+
```

#### 3.6.2 Column Projection

```sql
sql> SELECT name, salary FROM employees;
  +-------+--------+
  | name  | salary |
  +-------+--------+
  | Alice |  75000 |
  | Bob   |  60000 |
  | Carol |  80000 |
  | Dave  |  65000 |
  +-------+--------+
```

#### 3.6.3 Aliases

```sql
sql> SELECT name AS employee_name, salary * 12 AS annual_salary FROM employees;
  +---------------+---------------+
  | employee_name | annual_salary |
  +---------------+---------------+
  | Alice         |        900000 |
  | Bob           |        720000 |
  | Carol         |        960000 |
  | Dave          |        780000 |
  +---------------+---------------+
```

#### 3.6.4 WHERE Filtering

```sql
sql> SELECT * FROM employees WHERE dept = 'Engineering';
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  1 | Alice | Engineering |  75000 |
  |  3 | Carol | Engineering |  80000 |
  +----+-------+-------------+--------+

sql> SELECT * FROM employees WHERE salary > 65000;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  1 | Alice | Engineering |  75000 |
  |  3 | Carol | Engineering |  80000 |
  +----+-------+-------------+--------+

sql> SELECT * FROM employees WHERE dept = 'Engineering' AND salary > 70000;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  3 | Carol | Engineering |  80000 |
  +----+-------+-------------+--------+
```

#### 3.6.5 DISTINCT

```sql
sql> SELECT DISTINCT dept FROM employees;
  +-------------+
  | dept        |
  +-------------+
  | Engineering |
  | Marketing   |
  +-------------+
```

### 3.7 UPDATE

**Syntax**
```sql
UPDATE table_name SET column = value [, column = value ...] WHERE condition;
```

**Example**
```sql
sql> UPDATE employees SET salary = 85000 WHERE name = 'Alice';
  OK

sql> SELECT name, salary FROM employees WHERE name = 'Alice';
  +-------+--------+
  | name  | salary |
  +-------+--------+
  | Alice |  85000 |
  +-------+--------+
```

### 3.8 DELETE

**Syntax**
```sql
DELETE FROM table_name WHERE condition;
```

**Example**
```sql
sql> DELETE FROM employees WHERE id = 4;
  OK

sql> SELECT COUNT(*) FROM employees;
  +-------+
  | count |
  +-------+
  |     3 |
  +-------+
```

### 3.9 JOIN

**Syntax**
```sql
SELECT ... FROM table1 [alias]
  [INNER|LEFT] JOIN table2 [alias]
  ON join_condition
```

**Setup**
```sql
sql> CREATE TABLE departments (id INT, name TEXT, budget INT);
  OK
sql> INSERT INTO departments VALUES (1, 'Engineering', 500000);
  OK
sql> INSERT INTO departments VALUES (2, 'Marketing', 300000);
  OK
```

**INNER JOIN**
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

**LEFT JOIN**
```sql
sql> SELECT e.name, d.budget FROM employees e LEFT JOIN departments d ON e.dept = d.name;
  +-------+--------+
  | name  | budget |
  +-------+--------+
  | Alice | 500000 |
  | Bob   | 300000 |
  | Carol | 500000 |
  +-------+--------+
```

### 3.10 GROUP BY & Aggregation

**Syntax**
```sql
SELECT cols, AGG_FUNC(expr) ... FROM table [WHERE ...] GROUP BY cols [HAVING condition]
```

**Aggregate Functions**

| Function | Description | Return Type |
|----------|-------------|-------------|
| `COUNT(*)` | Count all rows | INT64 |
| `COUNT(col)` | Count non-NULL values | INT64 |
| `SUM(col)` | Sum | INT64 |
| `AVG(col)` | Average | FLOAT64 |
| `MIN(col)` | Minimum | Same as input |
| `MAX(col)` | Maximum | Same as input |

**Example**
```sql
sql> SELECT dept, COUNT(*) AS cnt, AVG(salary) AS avg_sal, SUM(salary) AS total
   ... FROM employees GROUP BY dept;
  +-------------+-----+--------------+--------+
  | dept        | cnt | avg_sal      | total  |
  +-------------+-----+--------------+--------+
  | Engineering |   2 | 77500.000000 | 155000 |
  | Marketing   |   1 | 60000.000000 |  60000 |
  +-------------+-----+--------------+--------+
```

### 3.11 ORDER BY / LIMIT / OFFSET

**Syntax**
```sql
SELECT ... ORDER BY column [ASC|DESC] [, column [ASC|DESC] ...]
  [LIMIT row_count] [OFFSET row_count]
```

**Examples**

Sort:
```sql
sql> SELECT * FROM employees ORDER BY salary DESC;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  3 | Carol | Engineering |  80000 |
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  +----+-------+-------------+--------+
```

Multi-column sort + limit:
```sql
sql> SELECT * FROM employees ORDER BY dept ASC, salary DESC LIMIT 3;
  +----+-------+-------------+--------+
  | id | name  | dept        | salary |
  +----+-------+-------------+--------+
  |  3 | Carol | Engineering |  80000 |
  |  1 | Alice | Engineering |  75000 |
  |  2 | Bob   | Marketing   |  60000 |
  +----+-------+-------------+--------+
```

### 3.12 Transaction Control

**Syntax**
```sql
BEGIN;
-- DML statements ...
COMMIT;    -- or ROLLBACK;

SET TRANSACTION ISOLATION LEVEL { READ UNCOMMITTED | READ COMMITTED
                                | REPEATABLE READ | SERIALIZABLE };
```

#### Basic Transaction

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
```

Changes within a transaction are invisible to other sessions until COMMIT.

#### Rollback

```sql
sql> BEGIN;
  Transaction started with ID: 2
sql> DELETE FROM employees WHERE id = 5;
  OK
sql> ROLLBACK;
  Transaction rolled back
-- Delete is undone
```

#### Isolation Levels

| Level | Behavior |
|-------|----------|
| `READ UNCOMMITTED` | Reads latest version including uncommitted |
| `READ COMMITTED` | Per-statement snapshot; no dirty reads (default) |
| `REPEATABLE READ` | Per-transaction snapshot; no non-repeatable reads |
| `SERIALIZABLE` | All of the above + table-level SIREAD locks (phantom prevention) |

```sql
sql> SET TRANSACTION ISOLATION LEVEL READ COMMITTED;
  Isolation level updated
sql> SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
  Isolation level updated
```

Must be issued outside a transaction; takes effect on the next `BEGIN`.

### 3.13 EXPLAIN

**Syntax**
```sql
EXPLAIN select_statement;
```

Shows the physical execution plan as a PostgreSQL-style tree.

**Example**
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
```

### 3.14 EXPLAIN ANALYZE

**Syntax**
```sql
EXPLAIN ANALYZE select_statement;
```

Executes the query and collects runtime statistics.

**Example**
```sql
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

### 3.15 Prepared Statements

**Syntax**
```sql
PREPARE statement_name FROM 'sql_string';
EXECUTE statement_name;
DEALLOCATE PREPARE statement_name;
DEALLOCATE PREPARE ALL;
```

Pre-compiles a SQL statement into a physical plan. Subsequent EXECUTE calls skip parsing and optimization.

**Example**
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

**Notes**
- Single quotes inside SQL strings must be escaped as `''`
- DDL operations (CREATE/DROP TABLE/INDEX) invalidate all prepared statements
- Disconnecting clears all prepared statements

### 3.16 Users & Authentication

CoroDB uses **opt-in authentication**: by default there are no users, and all connections have unrestricted access. Once the first user is created, authentication is activated — all SQL commands (except `CREATE USER` and `AUTH`) require authentication first.

#### CREATE USER

**Syntax**
```sql
CREATE USER username 'password';
```

Creates a database user. Passwords are stored as SHA-256 hashes with a server-side salt — never in plaintext.

**Permissions**: The first user can be created without authentication. Subsequent user creation requires authentication as `admin`.

**Example**
```sql
-- Step 1: Create the first user (auth not yet active)
sql> CREATE USER admin 'secret123';
  CREATE USER

-- Auth is now activated! All subsequent commands require AUTH

-- Step 2: Authenticate
sql> AUTH admin 'secret123';
  AUTH OK

-- Step 3: Use the database normally
sql> SELECT 'hello' AS greeting;
  +----------+
  | greeting |
  +----------+
  | hello    |
  +----------+

-- Step 4: Create more users (requires admin auth)
sql> CREATE USER alice 'alice_pwd';
  CREATE USER
```

**Authentication failures**
```sql
-- Wrong password
sql> AUTH admin 'wrong';
ERROR: [Auth] Authentication failed for user: admin

-- SQL before authentication
sql> SELECT 1;
ERROR: [Auth] Not authenticated. Use AUTH <username> '<password>'
```

#### AUTH

**Syntax**
```sql
AUTH username 'password';
```

Authenticates as the specified user. On success, sets `authenticated = true` for the current session.

#### Complete Workflow

```
Fresh CoroDB deployment:
  ├── No users → all commands open
  ├── CREATE USER admin 'pwd'  → auth activated
  ├── AUTH admin 'pwd'  → authenticated
  ├── Run SQL normally
  └── CREATE USER other 'pwd'  → requires admin auth
```

#### Password Storage

Passwords are stored as `SHA-256("corodb_salt_v1" + password)` hex strings. For production, use bcrypt/scrypt/Argon2.

### 3.17 CHECKPOINT

**Syntax**
```sql
CHECKPOINT;
```

Forces the following operations:
1. Flush all MemTables to L0 SSTables
2. Run full-level Compaction
3. Truncate all WAL files
4. Ensure all data is durably persisted

After CHECKPOINT, copy the `data/` directory for a consistent backup.

**Example**
```sql
sql> CHECKPOINT;
  CHECKPOINT
```

### 3.18 SHOW STATUS

**Syntax**
```sql
SHOW STATUS;
```

Displays server runtime metrics.

**Example**
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
```

| Metric | Description |
|--------|-------------|
| `active_transactions` | Number of active transactions |
| `plan_cache_entries` | Cached plan count |
| `tables` | Registered table count |
| `storage_engine` | Storage engine type |
| `database_version` | Database version |

---

## 4. Data Types

### Complete Type Table

| Keyword | C++ Type | Size | Range |
|---------|----------|------|-------|
| `INT`, `INT64`, `BIGINT` | `int64_t` | 8 | [-9.2×10^18, 9.2×10^18] |
| `FLOAT`, `DOUBLE`, `FLOAT64` | `double` | 8 | IEEE 754, ~15 sig. digits |
| `TEXT`, `STRING`, `VARCHAR` | `std::string` | variable | UTF-8 |

### Type Promotion

- `INT` mixed with `FLOAT64` → promoted to `FLOAT64`
- Division (`/`) always returns `FLOAT64`
- Modulo (`%`) requires integer operands

---

## 5. Expressions & Operators

### Arithmetic

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `salary + 1000` |
| `-` | Subtraction | `salary - 500` |
| `*` | Multiplication | `salary * 12` |
| `/` | Division (returns FLOAT64) | `total / count` |
| `%` | Modulo (INT only) | `id % 2` |

### Comparison

| Operator | Description |
|----------|-------------|
| `=` | Equal |
| `<>` | Not equal |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less than or equal |
| `>=` | Greater than or equal |

### Logical

| Operator | Description |
|----------|-------------|
| `AND` | Logical AND |
| `OR` | Logical OR |
| `NOT` | Logical NOT |

### String Concatenation

```sql
sql> SELECT 'Hello, ' || name || '!' FROM employees WHERE id = 1;
  +----------------------------+
  | "Hello, " || name || "!"   |
  +----------------------------+
  | Hello, Alice!              |
  +----------------------------+
```

### Arithmetic Expressions

```sql
sql> SELECT name, salary, salary * 12 AS annual FROM employees;
  +-------+--------+--------+
  | name  | salary | annual |
  +-------+--------+--------+
  | Alice |  75000 | 900000 |
  | Bob   |  60000 | 720000 |
  | Carol |  80000 | 960000 |
  +-------+--------+--------+
```

---

## 6. Client (csql)

### Interactive Mode

```bash
./csql
```

- Each line is a complete SQL statement (newline-delimited)
- Type `exit` or `quit` to disconnect
- Blank lines are ignored
- Trailing semicolons `;` are optional and auto-stripped

### Single-Statement Mode

```bash
./csql -e "SELECT COUNT(*) FROM employees;"
```

### Pipe Mode

```bash
echo "SELECT * FROM employees;" | ./csql
cat script.sql | ./csql
```

### Output Format

- SELECT queries: ASCII table
- DML / DDL: `OK` confirmation
- Errors: `ERROR: <message>` prefix

---

## 7. Configuration Reference

File: `corodb.conf` (INI format with comments). Auto-generated on first server startup (`corodb_genconfig` is available for manual pre-creation, but optional). Restart the server after editing. 9 sections, 22 keys.

### `[server]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `port` | int | `4000` | TCP listen port (1–65535) |
| `data_dir` | string | `./data` | Data directory |
| `io_threads` | int | `0` | I/O threads (0 = CPU count) |
| `worker_threads` | int | `0` | Worker threads (0 = CPU count) |
| `max_connections` | int | `10000` | Max concurrent connections |
| `reuse_port` | bool | `true` | SO_REUSEPORT |
| `idle_timeout_sec` | int | `0` | Idle timeout in seconds (0 = disabled) |
| `statement_timeout_ms` | int | `0` | Per-statement timeout in ms (0 = disabled) |

### `[storage]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `page_size` | int | `8192` | Page size (bytes) |
| `buffer_pages` | int | `256` | Buffer pool capacity (pages) |
| `memtable_size_bytes` | int | `1048576` | MemTable flush threshold (1 MB) |

### `[wal]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `sync_mode` | string | `fast` | Sync mode: `fast` (OS cache flush) / `durable` (kernel fsync, ACID) |
| `group_commit_delay_us` | int | `1000` | Group commit max wait (µs). 0 = immediate fsync |
| `group_commit_batch_size` | int | `64` | Batch size. 1 = strict mode (fsync per record) |

### `[connection]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `max_buffer_size` | int | `67108864` | Per-connection buffer limit (bytes, 64 MB) |

### `[network]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `send_timeout_ms` | int | `30000` | Client send timeout (ms) |
| `read_timeout_ms` | int | `30000` | Client response read timeout (ms) |

### `[lock_manager]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `timeout_ms` | int | `5000` | Table lock wait timeout (ms) |

### `[lsm]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `l0_compaction_threshold_bytes` | int | `65536` | L0 compaction trigger size (bytes) |
| `max_level` | int | `3` | Max LSM levels (1–7, including L0) |
| `sst_cache_entries` | int | `256` | Decoded SSTable cache capacity (LRU eviction) |

### `[plan_cache]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `max_entries` | int | `128` | Plan cache capacity (SELECT only) |

### `[thread_pool]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `max_queue_size` | int | `0` | Task queue limit (0 = unlimited), blocks submitter when full |

### `[auth]`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `password_salt` | string | `corodb_salt_v1` | Password hash salt; changing invalidates all existing passwords |

---

## 8. Query Optimizer & Execution Plans

### 8.1 Pipeline

```
SQL text → Parser (AST) → LogicalPlanner (logical plan) → RuleSet (5-rule fixed-point) → PhysicalPlanner (physical plan) → Executor
```

### 8.2 Logical Planning

The logical planner builds the plan tree bottom-up:

```
SELECT:  Scan → Join(s) → Filter → Aggregate → Sort → Limit → Project
INSERT:  Values → Insert
UPDATE:  Scan → Filter → Update
DELETE:  Scan → Filter → Delete
```

- `SELECT *` expands to all columns from all source tables
- Multiple JOINs are left-associative
- GROUP BY / HAVING / ORDER BY nodes are only added when present in the SQL

### 8.3 Five Rewrite Rules

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

Multi-table distribution:
```sql
SELECT * FROM orders o JOIN products p ON o.pid = p.id
WHERE o.amount > 100 AND p.price < 500;
```
After: `o.amount > 100` pushed left, `p.price < 500` pushed right.

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

After: inner Project absorbed.
```
Project [id, name]
  ->  Scan on employees
```

---

#### R3 Constant Folding

Evaluates expressions where all operands are literals at compile time.

**Supports**: arithmetic (+, -, *, /, %), string concat (||). NULL operands short-circuit to NULL.

```sql
SELECT * FROM employees WHERE salary > 50000 + 30000;
```

Before:
```
Filter: (salary > (50000 + 30000))
  ->  Scan on employees
```

After: `50000 + 30000 = 80000` computed at optimization time.
```
Filter: (salary > 80000)
  ->  Scan on employees
```

---

#### R4 Column Pruning

Propagates required column sets top-down. Projects output only columns actually needed upstream.

**Direction**: root → leaves. Root starts with `needed = nullopt` (keep all).

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

---

#### R5 Join Reorder

Swaps INNER JOIN subtrees so the smaller estimated side is on the left.

**Cost estimate**: `Scan = 1`, `Join = max(left, right) + 1`, `Aggregate = (child + 1) / 2`. No statistics needed.

**Trigger**: INNER JOIN with right subtree estimate < left subtree.

```sql
SELECT * FROM (SELECT * FROM a JOIN b ON a.id = b.id) ab
JOIN c ON a.id = c.id;
```

Before: left `(a JOIN b)` size=2, right `c` size=1. Triggers swap.
```
Join (INNER)
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
  ->  Scan on c             ← size=1
```

After:
```
Join (INNER)
  ->  Scan on c             ← size=1
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
```

### 8.4 Physical Operator Selection

The physical planner makes three key decisions:

**Scan**: `WHERE col = literal` + index → **IndexScan**; else **SeqScan + Filter**

**Join**: Equi-join + both sides sorted with matching key → **Merge Join** (skip re-sort); equi-join unsorted → **Hash Join**; non-equi → **Nested Loop Join**

**Aggregate**: Child Sort matches GROUP BY → **Sort Aggregate** (absorbs Sort, O(1) memory); else **Hash Aggregate**

**Aggregate Selection**:
- Child is Sort with columns matching GROUP BY → **Sort Aggregate** (absorb Sort, streaming O(1))
- Otherwise → **Hash Aggregate** (in-memory hash table)

### 8.5 Reading Execution Plans

The EXPLAIN plan tree reads from top (outermost output) to bottom (data source).

**Indentation rules**:
- 2 spaces + `->  ` per nesting level indicates parent → child
- Same indentation level = sibling subtrees (e.g., Join's left and right sides)
- `Sort Key:` / `Filter:` / `Group Key:` etc. are node attributes, indented 4 spaces past the node name

**Operator reference**:

| Operator | Meaning |
|----------|---------|
| `Seq Scan on t` | Full sequential scan of table t |
| `Index Scan on t` | Index-based lookup (requires index + equality WHERE) |
| `Filter: (cond)` | Evaluate condition on input rows |
| `Project [cols]` | Column projection output |
| `Hash Join (inner/left)` | Hash-based join |
| `Merge Join (inner/left)` | Merge join (inputs pre-sorted) |
| `Nested Loop (inner/left)` | Nested loop join |
| `Hash Aggregate` | In-memory hash aggregation |
| `Aggregate (sort)` | Streaming sort-based aggregation |
| `Sort` | Sort input rows |
| `Limit (rows=N)` | Limit output rows |

### 8.6 Plan Examples

#### Filter + Sort

```sql
EXPLAIN SELECT * FROM employees WHERE salary > 70000 ORDER BY name;
```

```
Project [id AS id, name AS name, dept AS dept, salary AS salary]
  ->  Sort
      Sort Key: name ASC
    ->  Filter: (salary > 70000)
      ->  Seq Scan on employees
```

Reading: Scan employees → filter salary>70000 → sort by name → project all columns.

#### JOIN

```sql
EXPLAIN SELECT e.name, d.budget FROM employees e INNER JOIN departments d ON e.dept = d.name;
```

```
Project [e.name AS name, d.budget AS budget]
  ->  Hash Join (inner)
      Hash Cond: (e.dept = d.name)
    ->  Seq Scan on employees
    ->  Seq Scan on departments
```

Reading: Scan employees (Build side, hash dept) → scan departments (Probe side, match on name) → Hash Join → project name and budget.

#### Aggregation

```sql
EXPLAIN SELECT dept, AVG(salary) FROM employees GROUP BY dept;
```

```
Hash Aggregate
    Group Key: dept
  ->  Seq Scan on employees
```

Reading: Full scan → hash aggregate by dept, compute AVG(salary) per group.

#### Index Acceleration

```sql
CREATE INDEX idx_dept ON employees (dept);
EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
```

```
Index Scan on employees
  Index Cond: (dept = 'Engineering')
```

Reading: Physical planner detected equality + index, replaced SeqScan+Filter with IndexScan. Uses the index to locate rows directly.

## 9. Architecture Overview

### System Architecture

```
Client (csql) ──TCP──> ReactorServer ──> ThreadPool
                                            │
                       ┌────────────────────┤
                       ▼                    ▼
                QueryProcessor        TransactionManager
                 parse→optimize        LockManager (timeout)
                       │               RowLockManager
                       ▼                    │
                Executor (C++23            │
                std::generator)            │
                       │                    │
                       ▼                    ▼
                LSM-Tree Engine ◄──────────┘
                 WAL + MemTable
                 + SSTable L0-L3
                 + BufferPool
                 + Compaction
                       │
                       ▼
                  data/ (on-disk files)
```

### Storage Layout

```
data/
  employees.wal           — Write-Ahead Log
  employees.lsm.L0        — SSTable Level 0 (newest)
  employees.lsm.L1        — SSTable Level 1
  employees.lsm.L2        — SSTable Level 2
  employees.lsm.L3        — SSTable Level 3 (oldest, GC target)
  employees.dept.idx      — Secondary index on dept column
  employees.idx_names     — Index name registry
```

### Write Path

```
INSERT → Parser → LogicalPlanner → PhysicalPlanner → Executor
  → WAL.append (type + len + checksum + payload)
  → WAL fsync (Group Commit batch)
  → MemTable.emplace (MVCCKey(pk, commit_ts), row)
  → When MemTable ≥ 1 MB:
      → flush: merge MemTable with L0
      → write_sstable(L0): .tmp → fsync → rename
      → WalManager.truncate: truncate old WAL + fsync
      → background Compaction: L0→L1→L2→L3
```

### Read Path

```
SELECT → Parser → LogicalPlanner → PhysicalPlanner → Executor
  → compute snapshot_ts
  → scan_visible(snapshot_ts):
      1. MemTable: for each pk, pick newest version with commit_ts ≤ snapshot_ts
      2. For each SSTable level:
         a. Check Bloom Filter → skip if pk definitely absent
         b. Binary search to locate pk start
         c. Pick first version with commit_ts ≤ snapshot_ts
      3. Merge all levels; per pk, pick highest commit_ts version
      4. Filter out tombstones (deletion markers)
  → Return rows via std::generator co_yield
```

### Compaction Strategy

- **Trigger**: L0 file ≥ 64 KB
- **Algorithm**: Leveled Compaction, L_n merged into L_n+1
- **Merge**: Two-way merge (pk ASC, commit_ts DESC), upper level wins
- **GC**: Runs at every level. Discards versions with commit_ts ≤ gc_horizon that are not the newest visible
- **Tombstone cleanup**: Orphaned tombstones (no newer version) are dropped entirely
- **Mutual exclusion**: Only one compaction per table at a time (try_lock)
- **Max level**: L3 (4 levels: L0-L3)

---

## 10. Known Limitations

| Feature | Status | Notes |
|---------|--------|-------|
| Subqueries | Not supported | `WHERE x IN (SELECT ...)` fails to parse |
| Range IndexScan | Not supported | Index only used for equality `col = value` |
| `IS NULL` | Not supported | No NULL literal support |
| RIGHT/FULL JOIN | Not supported | INNER and LEFT only |
| SAVEPOINT | Not supported | No nested transactions / partial rollback |
| TLS encryption | Not supported | Cleartext; protect via VPN/firewall |
| Multi-column indexes | Not supported | One column per index |
| Foreign keys | Not supported | No referential integrity checks |
