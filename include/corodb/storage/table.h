// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file table.h @brief Table 与 Catalog 类型定义。 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "corodb/common/types.h"


namespace corodb {

    /** @brief Value 的哈希器。 */
    struct ValueHash {
        [[nodiscard]] std::size_t operator()(const Value& v) const;
    };

    /** @brief Value 的相等比较器。 */
    struct ValueEq {
        [[nodiscard]] bool operator()(const Value& a, const Value& b) const;
    };

    /** @brief Value 的全序比较器（用于有序二级索引，支持范围扫描）。 */
    struct ValueLess {
        [[nodiscard]] bool operator()(const Value& a, const Value& b) const;
    };

    class StorageEngine;

    /** @brief 列定义。 */
    struct Column {
        std::string table;
        std::string name;
        TypeKind type;
        Oid column_oid{ 0 };
        Oid table_oid{ 0 };
        bool not_null{ false };    ///< NOT NULL 约束
        bool primary_key{ false }; ///< PRIMARY KEY 约束（隐含 NOT NULL）

        [[nodiscard]] std::string qualified_name() const {
            return table.empty() ? name : table + "." + name;
        }

        [[nodiscard]] bool has_valid_oid() const noexcept {
            return column_oid != 0;
        }
    };

    /** @brief 一行值数据。 */
    struct Row {
        std::vector<Value> values;

        [[nodiscard]] std::size_t size() const noexcept {
            return values.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return values.empty();
        }

        [[nodiscard]] const Value& operator[](std::size_t idx) const {
            return values[idx];
        }

        [[nodiscard]] Value& operator[](std::size_t idx) {
            return values[idx];
        }
    };

    /** @brief 表对象，管理模式、行缓存和二级索引。 */
    class Table {
    public:
        /** @brief 创建表对象，必要时从存储层加载状态。 */
        Table(std::string name, std::vector<Column> columns, StorageEngine* storage = nullptr);

        // 禁用复制（表管理资源和存储引擎引用）
        Table(const Table&) = delete;
        Table& operator=(const Table&) = delete;

        // 允许移动
        Table(Table&&) = default;
        Table& operator=(Table&&) = default;

        ~Table() = default;

        [[nodiscard]] const std::string& name() const noexcept {
            return name_;
        }

        [[nodiscard]] Oid oid() const noexcept {
            return oid_;
        }

        [[nodiscard]] const std::vector<Column>& columns() const noexcept {
            return columns_;
        }

        [[nodiscard]] std::size_t column_count() const noexcept {
            return columns_.size();
        }

        [[nodiscard]] std::size_t row_count() const noexcept {
            return rows_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return rows_.empty();
        }

        [[nodiscard]] std::optional<std::size_t> find_column(const std::string& column_name) const;

        /** @brief 插入一行并同步更新索引。 */
        void insert(Row row);

        /** @brief 批量插入多行。 */
        void insert_batch(std::vector<Row> rows);

        /** @brief 批量插入，并显式携带提交时间戳。 */
        void insert_batch(std::vector<Row> rows, uint64_t commit_ts);

        /** @brief 将当前表内容整表写回存储层。 */
        void rewrite_all();

        /** @brief 以增量方式持久化一行 upsert。 */
        void persist_row_upsert(const Row& row, uint64_t commit_ts = 0);

        /** @brief 以增量方式持久化主键删除。 */
        void persist_row_delete(int64_t key, uint64_t commit_ts = 0);

        /** @brief 重建当前表的全部索引。 */
        void refresh_indexes();

        /** @brief 为指定列创建索引，使用显式索引名（如 `idx_emp_id`）。 */
        void create_index(const std::string& index_name, const std::string& column);

        /** @brief 为指定列创建索引，自动生成索引名 `idx_{table}_{column}`。 */
        void create_index(const std::string& column);

        /**
         * @brief 按索引名删除索引。
         *
         * @param index_name 创建时使用的索引名（如 `idx_emp_id`）。
         * @return true 如果索引被成功删除，false 如果索引不存在。
         */
        bool drop_index(const std::string& index_name);

        [[nodiscard]] bool has_index(const std::string& column) const;

        /** @brief 使用列索引查找匹配行的主键集合（去重）。 */
        [[nodiscard]] std::vector<int64_t> lookup_index(const std::string& column, const Value& key) const;

        /** @brief 范围查找：返回索引列处于 [low, high]（按 inclusive 标志）区间内的主键集合（去重）。 */
        [[nodiscard]] std::vector<int64_t> lookup_index_range(const std::string& column,
                                                              const std::optional<Value>& low, bool low_inclusive,
                                                              const std::optional<Value>& high,
                                                              bool high_inclusive) const;

        [[nodiscard]] const std::vector<Row>& rows() const noexcept {
            return rows_;
        }

        /** @brief 读取给定快照时间点可见的全部行。 */
        [[nodiscard]] std::vector<Row> scan_visible(uint64_t snapshot_ts) const;

        /** @brief 读取给定快照时间点主键可见的最新版本。 */
        [[nodiscard]] std::optional<Row> lookup_visible(int64_t pk, uint64_t snapshot_ts) const;

        /** @brief 返回可变行缓存；调用方负责维护索引一致性。 */
        [[nodiscard]] std::vector<Row>& rows_mut() noexcept {
            return rows_;
        }

        [[nodiscard]] bool loaded_from_storage() const noexcept {
            return loaded_from_storage_;
        }

        [[nodiscard]] bool newly_created() const noexcept {
            return newly_created_;
        }

        [[nodiscard]] const std::unordered_set<std::string>& indexed_columns() const noexcept {
            return indexed_columns_;
        }

        /** @brief 单调递增的写计数器（用于 Serializable 幻读检测）。 */
        [[nodiscard]] uint64_t write_counter() const noexcept {
            return write_counter_.load(std::memory_order_acquire);
        }

        /** @brief 每次写入时调用，递增写计数器。 */
        void bump_write_counter() noexcept {
            write_counter_.fetch_add(1, std::memory_order_release);
        }

    private:
        Oid oid_;
        std::string name_;
        std::vector<Column> columns_;
        std::vector<Row> rows_;
        std::atomic<uint64_t> write_counter_{ 0 };
        StorageEngine* storage_{ nullptr };
        bool loaded_from_storage_{ false };
        bool newly_created_{ false };

        std::unordered_set<std::string> indexed_columns_;
        /// 二级索引：列名 → (列值 → 主键)，有序 multimap（支持等值与范围查找），与内存行缓存解耦。
        std::unordered_map<std::string, std::multimap<Value, int64_t, ValueLess>> indexes_;
        std::unordered_map<std::string, std::string> index_name_registry_; ///< 索引名 → 列名

        void index_row(const Row& row);

        void update_indexes_for_row(std::size_t rid);

        void rebuild_index_for_column(const std::string& column, std::size_t col_idx);
    };

    /** @brief 按名称和 OID 管理表对象。 */
    class Catalog {
    public:
        Catalog() = default;

        /** @brief 注册表到目录；若 table 为空则忽略。 */
        void register_table(std::shared_ptr<Table> table);

        /** @brief 按名称注销表；返回是否成功删除。 */
        bool unregister_table(const std::string& name);

        /** @brief 按名称查找表；未找到返回 nullptr。 */
        [[nodiscard]] std::shared_ptr<Table> lookup(const std::string& name) const;

        /** @brief 按 OID 查找表；未找到返回 nullptr。 */
        [[nodiscard]] std::shared_ptr<Table> lookup(Oid oid) const;

        /** @brief 判断目录中是否存在指定名称的表。 */
        [[nodiscard]] bool contains(const std::string& name) const;

        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] bool empty() const noexcept;

        /** @brief 返回所有已注册表的名称列表。 */
        [[nodiscard]] std::vector<std::string> table_names() const;

        /** @brief 清空目录中的所有表。 */
        void clear();

    private:
        std::unordered_map<std::string, std::shared_ptr<Table>> tables_by_name_;
        std::unordered_map<Oid, std::shared_ptr<Table>> tables_by_oid_;
    };

} // namespace corodb
