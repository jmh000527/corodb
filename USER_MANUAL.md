# CoroDB 用户手册

> **版本**: 0.1.0 | **项目**: [github.com/corodb/corodb](https://github.com/corodb/corodb) | **许可证**: MIT

## 目录

1. [安装与构建](#1-安装与构建)
2. [快速入门](#2-快速入门)
3. [SQL 命令参考](#3-sql-命令参考)
   - [3.1 CREATE TABLE](#31-create-table)
   - [3.2 DROP TABLE](#32-drop-table)
   - [3.3 CREATE INDEX](#33-create-index)
   - [3.4 DROP INDEX](#34-drop-index)
   - [3.5 INSERT](#35-insert)
   - [3.6 SELECT](#36-select)
   - [3.7 UPDATE](#37-update)
   - [3.8 DELETE](#38-delete)
   - [3.9 JOIN](#39-join)
   - [3.10 GROUP BY 与聚合](#310-group-by-与聚合)
   - [3.11 ORDER BY / LIMIT / OFFSET](#311-order-by--limit--offset)
   - [3.12 事务控制](#312-事务控制)
   - [3.13 EXPLAIN](#313-explain)
   - [3.14 EXPLAIN ANALYZE](#314-explain-analyze)
   - [3.15 预处理语句](#315-预处理语句)
   - [3.16 AUTH 认证](#316-auth-认证)
   - [3.17 CHECKPOINT](#317-checkpoint)
   - [3.18 SHOW STATUS](#318-show-status)
4. [数据类型](#4-数据类型)
5. [表达式与运算符](#5-表达式与运算符)
6. [客户端 csql](#6-客户端-csql)
7. [配置参考](#7-配置参考)
8. [查询优化器与执行计划](#8-查询优化器与执行计划)
   - [8.1 优化流水线](#81-优化流水线)
   - [8.2 逻辑规划](#82-逻辑规划)
   - [8.3 五条重写规则](#83-五条重写规则)
   - [8.4 物理算子选择](#84-物理算子选择)
   - [8.5 执行计划阅读指南](#85-执行计划阅读指南)
   - [8.6 典型执行计划示例](#86-典型执行计划示例)
9. [架构概览](#9-架构概览)
10. [已知限制](#10-已知限制)

---

## 1. 安装与构建

### 环境要求

| 平台 | 编译器 | 工具 |
|------|--------|------|
| Windows | MSVC 2026+ (C++23) | CMake 3.26+ |
| Linux | GCC 16+ / Clang 20+ | CMake 3.26+ |
| macOS | Clang 20+ | CMake 3.26+ |

Google Test 1.14.0 通过 CMake `FetchContent` 自动下载，无需手动安装。

### 构建步骤

```bash
# 1. 克隆项目
git clone <repo-url> && cd corodb

# 2. 配置 CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. 编译
cmake --build build --config Release -j

# 4. 产物位置
#   bin/corodb_server      — 数据库服务器
#   bin/csql               — 命令行客户端
#   bin/corodb_genconfig   — 配置文件生成器
#   bin/bench_concurrency  — 并发性能测试
```

### 运行测试

```bash
cd build && ctest -j8 --output-on-failure
```

---

## 2. 快速入门

### 生成默认配置

```bash
cd bin
# 首次启动会自动生成 corodb.conf，也可预先手动创建（非必须）：
# ./corodb_genconfig
```

### 启动服务器

```bash
./corodb_server
```

```
[2026-05-23 05:59:33] [INFO] Initializing thread-safe database...
[2026-05-23 05:59:33] [INFO] Database initialized with 16 worker threads
[2026-05-23 05:59:33] [INFO] ReactorServer initialized: port=4000
[2026-05-23 05:59:33] [INFO] ReactorServer started on port 4000
```

### 连接数据库

```bash
# 交互模式
./csql

# 单语句模式
./csql -e "SELECT 1;"

# 管道模式
echo "CREATE TABLE t (id INT);" | ./csql
```

连接成功提示：
```
Connected to 127.0.0.1:4000 (type exit/quit to leave)
sql>
```

### 第一个查询

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

## 3. SQL 命令参考

### 3.1 CREATE TABLE

**语法**
```sql
CREATE TABLE table_name (
    column_name type_keyword,
    column_name type_keyword,
    ...
);
```

**支持的类型关键字**

| 关键字 | 存储类型 | 说明 |
|--------|----------|------|
| `INT`, `INT64`, `BIGINT` | 64 位有符号整数 | 范围 [-2^63, 2^63-1] |
| `FLOAT`, `DOUBLE`, `FLOAT64` | IEEE 754 双精度浮点 | 约 15 位有效数字 |
| `TEXT`, `STRING`, `VARCHAR` | UTF-8 字符串 | 变长 |

**示例**
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

**语法**
```sql
DROP TABLE table_name;
```

删除表及其所有关联文件（WAL、SSTable 各级、索引文件）。

**示例**
```sql
sql> DROP TABLE products;
  OK
```

### 3.3 CREATE INDEX

**语法**
```sql
CREATE INDEX index_name ON table_name (column_name);
```

对指定列创建二级索引。创建时会对当前数据做全量扫描构建索引，后续写入由系统维护。索引用于加速等值查询（`col = value`）。

**示例**
```sql
sql> CREATE INDEX idx_emp_dept ON employees (dept);
  OK

sql> EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
-- 物理优化器会自动将 SeqScan 替换为 IndexScan
```

### 3.4 DROP INDEX

**语法**
```sql
DROP INDEX index_name;
```

**示例**
```sql
sql> DROP INDEX idx_emp_dept;
  OK
```

### 3.5 INSERT

**语法**
```sql
INSERT INTO table_name VALUES (value1, value2, ...);
```

值的顺序必须与表定义中列的顺序一致，数量必须匹配。

**示例**
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

**完整语法**
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

#### 3.6.1 基础查询

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

#### 3.6.2 列投影

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

#### 3.6.3 别名

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

#### 3.6.4 WHERE 条件过滤

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

**语法**
```sql
UPDATE table_name SET column = value [, column = value ...] WHERE condition;
```

**示例**
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

**语法**
```sql
DELETE FROM table_name WHERE condition;
```

**示例**
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

**语法**
```sql
SELECT ... FROM table1 [alias]
  [INNER|LEFT] JOIN table2 [alias]
  ON join_condition
```

**示例**

先准备数据：
```sql
sql> CREATE TABLE departments (id INT, name TEXT, budget INT);
  OK
sql> INSERT INTO departments VALUES (1, 'Engineering', 500000);
  OK
sql> INSERT INTO departments VALUES (2, 'Marketing', 300000);
  OK
```

INNER JOIN：
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

LEFT JOIN：
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

### 3.10 GROUP BY 与聚合

**语法**
```sql
SELECT cols, AGG_FUNC(expr) ... FROM table [WHERE ...] GROUP BY cols [HAVING condition]
```

**聚合函数**

| 函数 | 说明 | 返回类型 |
|------|------|----------|
| `COUNT(*)` | 计数所有行 | INT64 |
| `COUNT(col)` | 计数非 NULL 值 | INT64 |
| `SUM(col)` | 求和 | INT64 |
| `AVG(col)` | 平均值 | FLOAT64 |
| `MIN(col)` | 最小值 | 与输入相同 |
| `MAX(col)` | 最大值 | 与输入相同 |

**示例**
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

**语法**
```sql
SELECT ... ORDER BY column [ASC|DESC] [, column [ASC|DESC] ...]
  [LIMIT row_count] [OFFSET row_count]
```

**示例**

排序：
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

多列排序 + 限制行数：
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

### 3.12 事务控制

**语法**
```sql
BEGIN;
-- DML 语句 ...
COMMIT;    -- 或 ROLLBACK;

SET TRANSACTION ISOLATION LEVEL { READ UNCOMMITTED | READ COMMITTED
                                | REPEATABLE READ | SERIALIZABLE };
```

#### 基本事务

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

事务内修改在 COMMIT 前对其他会话不可见。

#### 回滚

```sql
sql> BEGIN;
  Transaction started with ID: 2
sql> DELETE FROM employees WHERE id = 5;
  OK
sql> ROLLBACK;
  Transaction rolled back
-- 删除操作被撤销
```

#### 隔离级别

| 级别 | 行为 |
|------|------|
| `READ UNCOMMITTED` | 读取最新版本，含未提交数据 |
| `READ COMMITTED` | 每条语句获取新快照，防止脏读（默认） |
| `REPEATABLE READ` | 整个事务使用同一快照，防止不可重复读 |
| `SERIALIZABLE` | 上述所有 + 表级 SIREAD 锁（防止幻读） |

```sql
sql> SET TRANSACTION ISOLATION LEVEL READ COMMITTED;
  Isolation level updated

sql> SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
  Isolation level updated
```

必须在事务外设置，对后续 `BEGIN` 生效。

### 3.13 EXPLAIN

**语法**
```sql
EXPLAIN select_statement;
```

显示查询的物理执行计划（PostgreSQL 风格树形结构）。

**示例**
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

**语法**
```sql
EXPLAIN ANALYZE select_statement;
```

实际执行查询并收集运行时统计（算子类型、行数、耗时）。

**示例**
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

### 3.15 预处理语句

**语法**
```sql
PREPARE statement_name FROM 'sql_string';
EXECUTE statement_name;
DEALLOCATE PREPARE statement_name;
DEALLOCATE PREPARE ALL;
```

将 SQL 语句预编译为物理计划并缓存，后续 EXECUTE 跳过解析和优化阶段。

**示例**
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

**注意事项**
- SQL 字符串内的单引号需转义为 `''`
- DDL 操作（CREATE/DROP TABLE/INDEX）会自动清空所有预处理语句
- 会话断开时自动清空

### 3.16 用户与认证

CoroDB 采用**选择性认证**机制：默认无用户，所有连接自动放行。一旦创建了第一个用户，认证即被激活——所有 SQL 命令（除 `CREATE USER` 和 `AUTH` 外）都必须先通过认证。

#### CREATE USER

**语法**
```sql
CREATE USER username 'password';
```

创建数据库用户。密码使用 SHA-256 + 服务端 salt 哈希存储，不明文保存。

**权限**：第一个用户创建时无需认证。后续创建用户需要以 `admin` 身份认证。

**示例**
```sql
-- 第 1 步：创建第一个用户（此时认证尚未激活）
sql> CREATE USER admin 'secret123';
  CREATE USER

-- 此时认证已被激活！后续所有命令需要 AUTH

-- 第 2 步：认证
sql> AUTH admin 'secret123';
  AUTH OK

-- 第 3 步：正常使用
sql> SELECT 'hello' AS greeting;
  +----------+
  | greeting |
  +----------+
  | hello    |
  +----------+

-- 第 4 步：创建更多用户（需要 admin 权限）
sql> CREATE USER alice 'alice_pwd';
  CREATE USER
```

**认证失败**
```sql
-- 错误密码
sql> AUTH admin 'wrong';
ERROR: [Auth] Authentication failed for user: admin

-- 认证前尝试执行 SQL
sql> SELECT 1;
ERROR: [Auth] Not authenticated. Use AUTH <username> '<password>'
```

#### AUTH

**语法**
```sql
AUTH username 'password';
```

使用指定用户身份认证。成功后在当前会话中设置 `authenticated = true`。

#### 完整工作流

```
新部署的 CoroDB:
  ├── 无用户 → 所有命令开放
  ├── CREATE USER admin 'pwd'  → 认证激活
  ├── AUTH admin 'pwd'  → 认证通过
  ├── 正常执行 SQL
  └── CREATE USER other 'pwd'  → 需要 admin 认证
```

#### 密码存储

密码不存储明文。存储格式：`SHA-256("corodb_salt_v1" + password)` 的 16 进制字符串。生产环境建议使用 bcrypt/scrypt/Argon2。

### 3.17 CHECKPOINT

**语法**
```sql
CHECKPOINT;
```

强制执行以下操作：
1. 刷新所有表的 MemTable 到 L0 SSTable
2. 运行全层级 Compaction
3. 截断所有 WAL 文件
4. 确保所有数据持久化

执行完毕后可直接拷贝 `data/` 目录做一致性备份。

**示例**
```sql
sql> CHECKPOINT;
  CHECKPOINT
```

### 3.18 SHOW STATUS

**语法**
```sql
SHOW STATUS;
```

显示服务器运行时指标。

**示例**
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

| 指标 | 说明 |
|------|------|
| `active_transactions` | 当前活跃事务数 |
| `plan_cache_entries` | 计划缓存条目数 |
| `tables` | 已注册的表数 |
| `storage_engine` | 存储引擎类型 |
| `database_version` | 数据库版本 |

---

## 4. 数据类型

### 完整类型表

| 关键字 | C++ 类型 | 字节 | 范围 |
|--------|----------|------|------|
| `INT`, `INT64`, `BIGINT` | `int64_t` | 8 | [-9.2×10^18, 9.2×10^18] |
| `FLOAT`, `DOUBLE`, `FLOAT64` | `double` | 8 | IEEE 754, ~15 位有效数字 |
| `TEXT`, `STRING`, `VARCHAR` | `std::string` | 变长 | UTF-8 编码 |

### 类型提升规则

- `INT` 与 `FLOAT64` 混合运算 → 提升为 `FLOAT64`
- 除法 (`/`) 始终返回 `FLOAT64`
- 取模 (`%`) 仅支持 `INT` 操作数

---

## 5. 表达式与运算符

### 算术运算符

| 运算符 | 说明 | 示例 |
|--------|------|------|
| `+` | 加法 | `salary + 1000` |
| `-` | 减法 | `salary - 500` |
| `*` | 乘法 | `salary * 12` |
| `/` | 除法（返回 FLOAT64） | `total / count` |
| `%` | 取模（仅 INT） | `id % 2` |

### 比较运算符

| 运算符 | 说明 |
|--------|------|
| `=` | 等于 |
| `<>` | 不等于 |
| `<` | 小于 |
| `>` | 大于 |
| `<=` | 小于等于 |
| `>=` | 大于等于 |

### 逻辑运算符

| 运算符 | 说明 |
|--------|------|
| `AND` | 逻辑与 |
| `OR` | 逻辑或 |
| `NOT` | 逻辑非 |

### 字符串拼接

```sql
sql> SELECT 'Hello, ' || name || '!' FROM employees WHERE id = 1;
  +----------------------------+
  | "Hello, " || name || "!"   |
  +----------------------------+
  | Hello, Alice!              |
  +----------------------------+
```

### 算术表达式

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

## 6. 客户端 csql

### 交互模式

```bash
./csql
```

- 每行是一条完整 SQL 语句（换行符为分隔符）
- 输入 `exit` 或 `quit` 断开连接
- 空行被忽略
- 行末分号 `;` 可选，会被自动去除

### 单语句模式

```bash
./csql -e "SELECT COUNT(*) FROM employees;"
```

### 管道模式

```bash
echo "SELECT * FROM employees;" | ./csql

# 批量执行
cat script.sql | ./csql
```

### 输出格式

- SELECT 查询：ASCII 表格
- DML / DDL：`OK` 确认消息
- 错误：`ERROR: <消息>` 前缀

---

## 7. 配置参考

配置文件 `corodb.conf`，INI 格式，含注释。首次启动服务器时自动生成到可执行文件同目录（`corodb_genconfig` 可手动预先创建，非必须）。修改后重启服务生效。共 9 个段、22 个可配置项。

### `[server]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `port` | int | `4000` | TCP 监听端口（1–65535） |
| `data_dir` | string | `./data` | 数据文件存储目录 |
| `io_threads` | int | `0` | I/O 线程数（0 = 自动检测 CPU 核心数） |
| `worker_threads` | int | `0` | 工作线程数（0 = 自动检测 CPU 核心数） |
| `max_connections` | int | `10000` | 最大并发连接数 |
| `reuse_port` | bool | `true` | SO_REUSEPORT 端口复用 |
| `idle_timeout_sec` | int | `0` | 空闲连接超时秒数（0 = 禁用） |
| `statement_timeout_ms` | int | `0` | 单条语句超时毫秒数（0 = 禁用） |

### `[storage]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `page_size` | int | `8192` | 数据页大小（字节）。所有存储层共用 |
| `buffer_pages` | int | `256` | 缓冲池容量（页数）。总内存 = page_size × buffer_pages |
| `memtable_size_bytes` | int | `1048576` | MemTable 触发 flush 的字节阈值（1 MB） |

### `[wal]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `sync_mode` | string | `fast` | 同步模式：`fast`（仅 OS 缓存 flush，高性能）/ `durable`（内核 fsync，ACID） |
| `group_commit_delay_us` | int | `1000` | Group Commit 最长等待时间（微秒）。0 = 立即 fsync |
| `group_commit_batch_size` | int | `64` | 累积到此条数后触发批量 fsync。1 = 严格模式 |

### `[connection]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `max_buffer_size` | int | `67108864` | 单连接收发缓冲区上限（字节，默认 64 MB），超过强制断开 |

### `[network]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `send_timeout_ms` | int | `30000` | 客户端发送超时（毫秒） |
| `read_timeout_ms` | int | `30000` | 客户端接收响应超时（毫秒） |

### `[lock_manager]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `timeout_ms` | int | `5000` | 表级锁等待超时（毫秒），超时抛出异常 |

### `[lsm]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `l0_compaction_threshold_bytes` | int | `65536` | L0 文件大小阈值（字节），超过触发 Compaction |
| `max_level` | int | `3` | LSM 最大层级数（1–7，含 L0），控制树深度 |
| `sst_cache_entries` | int | `256` | SSTable 解码缓存容量（条目），LRU 淘汰 |

### `[plan_cache]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `max_entries` | int | `128` | 物理计划缓存容量（条目），仅缓存 SELECT |

### `[thread_pool]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `max_queue_size` | int | `0` | 任务队列最大长度（0 = 无限制），超过阻塞提交者 |

### `[auth]` 段

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `password_salt` | string | `corodb_salt_v1` | 密码哈希盐值，修改后已有密码全部失效 |

---

## 8. 查询优化器与执行计划

### 8.1 优化流水线

```
SQL文本 ──> Parser (AST) ──> LogicalPlanner (逻辑计划) ──> RuleSet (5规则定点迭代) ──> PhysicalPlanner (物理计划) ──> Executor
```

### 8.2 逻辑规划

逻辑规划器将 AST 按以下顺序自底向上构建逻辑计划树：

```
SELECT:  Scan → Join(s) → Filter → Aggregate → Sort → Limit → Project
INSERT:  Values → Insert
UPDATE:  Scan → Filter → Update
DELETE:  Scan → Filter → Delete
```

- `SELECT *` 会展开为所有源表的所有列
- 多个 JOIN 按左结合方式构建
- GROUP BY / HAVING / ORDER BY 仅在 SQL 中存在时才添加对应节点

### 8.3 五条重写规则

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
优化后：`o.amount > 100` 推到左侧，`p.price < 500` 推到右侧。

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

优化后：内层 Project 被吸收。
```
Project [id, name]
  ->  Scan on employees
```

---

#### R3 常量折叠（Constant Folding）

编译期计算所有操作数为字面量的表达式。

**支持**：算术运算（加减乘除取模）、字符串拼接（`||`）。涉及 NULL 短路为 NULL。除以零不折叠。

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

---

#### R4 列裁剪（Column Pruning）

从根节点向下传播所需列集合，裁剪 Project 只输出上游引用的列。

**方向**：自顶向下。根节点 `needed = nullopt`（保留所有），逐层收集列引用传递。

```sql
SELECT name FROM employees WHERE dept = 'Eng';
```

传播过程：
```
Project [name]                        ← needed = nullopt → 保留 name
  → 收集 {name}
  ->  Filter: (dept = 'Eng')          ← needed = {name}, 合并谓词列 {dept}
    → child_needed = {name, dept}
    ->  Scan on employees             ← 仅需 2 列（即使表有 20 列）
```

---

#### R5 连接重排序（Join Reorder）

对 INNER JOIN 交换左右子树，使估算较小的在左侧。

**代价估算**：`Scan = 1`，`Join = max(left, right) + 1`，`Aggregate = (child + 1) / 2`。无需统计信息。

**触发条件**：INNER JOIN 且右子树估算大小 < 左子树。

```sql
SELECT * FROM (SELECT * FROM a JOIN b ON a.id = b.id) ab
JOIN c ON a.id = c.id;
```

优化前：左侧 `(a JOIN b)` 大小=2，右侧 `c` 大小=1。触发交换。
```
Join (INNER)
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
  ->  Scan on c             ← size=1
```

优化后：
```
Join (INNER)
  ->  Scan on c             ← size=1
  ->  Join (INNER)          ← size=2
    ->  Scan on a
    ->  Scan on b
```

### 8.4 物理算子选择

物理规划器根据重写后的逻辑计划做出三个关键选择：

**Scan 选择**：
- `WHERE col = literal` + col 有索引 → **IndexScan**（直接定位，跳过全表扫描）
- 否则 → **SeqScan + Filter**

**Join 选择**：
- 等值连接 + 两侧均有 Sort 且首个排序列匹配连接键 → **Merge Join**（预排序优化，跳过重排）
- 等值连接 + 无匹配排序 → **Hash Join**（构建哈希表 + 探测）
- 非等值连接 → **Nested Loop Join**（逐行匹配）

**Aggregate 选择**：
- 子节点为 Sort 且排序列匹配 GROUP BY 列 → **Sort Aggregate**（吸收 Sort，流式 O(1) 内存）
- 否则 → **Hash Aggregate**（内存哈希表，所有组并存）

### 8.5 执行计划阅读指南

EXPLAIN 输出的计划树从上到下对应执行器的**外层到内层**（即从最终输出到原始数据源）。

**缩进含义**：
- 每层缩进 2 空格 + `->  ` 表示父子关系
- 同一缩进级别的多个节点是兄弟（如 Join 的左右子树）
- `Sort Key:` / `Filter:` / `Group Key:` 等是当前节点的属性，比节点名多缩进 4 空格

**算子名称解读**：

| 算子 | 含义 |
|------|------|
| `Seq Scan on t` | 全表顺序扫描表 t |
| `Index Scan on t` | 通过索引定位表 t 的行（需存在索引且 WHERE 为等值） |
| `Filter: (cond)` | 对输入行执行条件过滤 |
| `Project [cols]` | 列投影输出，方括号内是输出列列表 |
| `Hash Join (inner/left)` | 哈希连接，括号内为连接类型 |
| `Merge Join (inner/left)` | 归并连接（输入已排序时选用） |
| `Nested Loop (inner/left)` | 嵌套循环连接 |
| `Hash Aggregate` | 哈希聚合（所有组在内存中） |
| `Aggregate (sort)` | 排序聚合（流式，O(1) 内存） |
| `Sort` | 对输入排序 |
| `Limit (rows=N)` | 限制输出行数 |

### 8.6 典型执行计划示例

#### 示例 1：过滤 + 排序

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

解析：先全表扫描 employees → Filter 过滤 salary > 70000 的行 → Sort 按 name 排序 → Project 输出所有列。

#### 示例 2：JOIN + 投影

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

解析：先扫描 employees（Build 侧构建 dept 的哈希表）→ 扫描 departments（Probe 侧用 name 探测）→ Hash Join 匹配 → Project 输出 name 和 budget。

#### 示例 3：聚合 + 分组

```sql
EXPLAIN SELECT dept, AVG(salary) FROM employees GROUP BY dept;
```

```
Hash Aggregate
    Group Key: dept
  ->  Seq Scan on employees
```

解析：全表扫描 → Hash Aggregate 按 dept 分组，计算每组的 AVG(salary)。

#### 示例 4：索引加速

```sql
CREATE INDEX idx_dept ON employees (dept);
EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
```

```
Index Scan on employees
  Index Cond: (dept = 'Engineering')
```

解析：物理优化器检测到等值条件 + 索引存在，将 SeqScan + Filter 替换为 IndexScan。执行时通过索引直接定位 dept='Engineering' 的行，无需全表扫描。

## 9. 架构概览

### 系统架构

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
                  data/ (磁盘文件)
```

### 存储布局

```
data/
  employees.wal           — Write-Ahead Log
  employees.lsm.L0        — SSTable Level 0（最新）
  employees.lsm.L1        — SSTable Level 1
  employees.lsm.L2        — SSTable Level 2
  employees.lsm.L3        — SSTable Level 3（最旧，GC 目标）
  employees.dept.idx      — dept 列二级索引
  employees.idx_names     — 索引名注册表
```

### 写入路径

```
INSERT → Parser → LogicalPlanner → PhysicalPlanner → Executor
  → WAL.append (type + len + checksum + payload)
  → WAL fsync (Group Commit 批量)
  → MemTable.emplace (MVCCKey(pk, commit_ts), row)
  → 当 MemTable ≥ 1MB:
      → flush: MemTable 与 L0 归并
      → write_sstable(L0): .tmp → fsync → rename
      → WalManager.truncate: 截断旧 WAL + fsync
      → 后台 Compaction: L0→L1→L2→L3
```

### 读取路径

```
SELECT → Parser → LogicalPlanner → PhysicalPlanner → Executor
  → 计算 snapshot_ts
  → scan_visible(snapshot_ts):
      1. MemTable: 取各 pk commit_ts ≤ snapshot_ts 的最新版本
      2. 对每个 SSTable 层级:
         a. 检查 Bloom Filter → 如果 pk 确定不存在，跳过
         b. 否则二分定位 pk 起点
         c. 取首个 commit_ts ≤ snapshot_ts 的版本
      3. 合并所有层结果，每 pk 取最新 commit_ts 版本
      4. 过滤掉 tombstone（删除标记）
  → 通过 std::generator co_yield 逐行返回
```

### Compaction 策略

- **触发条件**: L0 文件 ≥ 64 KB
- **算法**: Leveled Compaction，L_n 与 L_n+1 归并
- **合并**: 两路归并 (pk ASC, commit_ts DESC)，上层覆盖下层
- **GC**: 每层都运行。丢弃 commit_ts ≤ gc_horizon 的非最新版本
- **Tombstone 清理**: 孤立 tombstone（无更新版本的删除标记）整段丢弃
- **互斥**: 单表同时只运行一个 Compaction（try_lock 跳过）
- **最大层级**: L3（4 层：L0-L3）

---

## 10. 已知限制

| 特性 | 状态 | 说明 |
|------|------|------|
| 子查询 | 不支持 | `WHERE x IN (SELECT ...)` 无法解析 |
| 范围 IndexScan | 不支持 | 索引仅用于等值条件 `col = value` |
| `IS NULL` | 不支持 | 缺少 NULL 字面量支持 |
| RIGHT/FULL JOIN | 不支持 | 仅支持 INNER 和 LEFT JOIN |
| SAVEPOINT | 不支持 | 无嵌套事务/部分回滚 |
| TLS 加密 | 不支持 | 明文传输，建议通过 VPN/防火墙保护 |
| 多列索引 | 不支持 | 每索引仅一列 |
| 外键约束 | 不支持 | 无引用完整性检查 |
