// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file table.cpp
// @brief 表数据文件读写操作的实现。

#include "corodb/storage/table.h"

#include <functional>
#include <stdexcept>

#include "corodb/storage/storage_engine.h"

namespace corodb {
    /**
     * @brief Value的哈希函数
     * @param v 要计算哈希值的Value对象
     * @return 计算得到的哈希值
     */
    std::size_t ValueHash::operator()(const Value& v) const {
        if (std::holds_alternative<NullValue>(v))
            return 0x9e3779b9; // 空值的哈希值
        if (std::holds_alternative<int64_t>(v))
            return std::hash<int64_t>{}(std::get<int64_t>(v)) ^ 0x9e3779b9;     // 整数的哈希值
        return std::hash<std::string>{}(std::get<std::string>(v)) ^ 0x85ebca6b; // 字符串的哈希值
    }

    /**
     * @brief Value的相等比较函数
     * @param a 第一个比较对象
     * @param b 第二个比较对象
     * @return 如果相等返回true，否则返回false
     */
    bool ValueEq::operator()(const Value& a, const Value& b) const {
        if (a.index() != b.index())
            return false; // 类型不同，不相等
        if (std::holds_alternative<NullValue>(a))
            return true; // 两个都是空值，相等
        if (std::holds_alternative<int64_t>(a))
            return std::get<int64_t>(a) == std::get<int64_t>(b);     // 比较整数值
        return std::get<std::string>(a) == std::get<std::string>(b); // 比较字符串值
    }

    /**
     * @brief Value 的全序比较（用于有序二级索引）。
     *
     * 先按 variant 序号排序（NullValue<int64<double<string），同类型内按值排序。
     * 对单列索引而言值类型一致，因此实际上是同类型比较。
     */
    bool ValueLess::operator()(const Value& a, const Value& b) const {
        if (a.index() != b.index())
            return a.index() < b.index();
        if (std::holds_alternative<int64_t>(a))
            return std::get<int64_t>(a) < std::get<int64_t>(b);
        if (std::holds_alternative<double>(a))
            return std::get<double>(a) < std::get<double>(b);
        if (std::holds_alternative<std::string>(a))
            return std::get<std::string>(a) < std::get<std::string>(b);
        return false; // NullValue 视为相等
    }

    /**
     * @brief Table class constructor
     *
     * This constructor initializes the table structure, handles interaction with the storage engine,
     * loads existing tables or creates new ones as needed, and loads any persisted secondary indexes.
     *
     * @param name Table name, used to identify the table in the storage engine
     * @param columns Table column definitions, including column names and data types
     * @param storage Storage engine pointer, responsible for table data persistence and retrieval
     */
    Table::Table(std::string name, std::vector<Column> columns, StorageEngine* storage)
        : oid_(generate_oid()), name_(std::move(name)), columns_(std::move(columns)), rows_(), storage_(storage) {
        // Assign Oid to all columns
        for (auto& column: columns_) {
            column.table_oid = oid_;
            column.column_oid = generate_oid();
        }
        // 如果没有提供存储引擎，标记为新创建并直接返回
        if (!storage_) {
            newly_created_ = true;
            return;
        }

        // 检查表是否已存在于存储中
        if (storage_->table_exists(name_)) {
            try {
                // 表存在，标记为从存储加载
                loaded_from_storage_ = true;
                // 从存储加载表结构（列定义）
                auto disk_columns = storage_->load_schema(name_);
                // 如果加载到有效的列定义，则替换传入的列定义
                if (!disk_columns.empty()) {
                    columns_ = std::move(disk_columns);
                }
                // 不再全量加载 rows_（去除内存天花板）：存储型表的读取一律经 scan_visible 流式获取。
            } catch (const std::exception&) {
                // 文件不兼容或损坏时，重新创建表
                storage_->create_table(name_, columns_);
                newly_created_ = true;
            }
        } else {
            // 表不存在，创建新表
            storage_->create_table(name_, columns_);
            newly_created_ = true;
        }

        // 加载所有已持久化的二级索引
        auto persisted = storage_->list_indexes(name_);
        for (const auto& col: persisted) {
            // 查找索引对应的列索引
            std::size_t col_idx = columns_.size(); // 初始化为无效索引
            for (std::size_t i = 0; i < columns_.size(); ++i) {
                if (columns_[i].name == col) {
                    col_idx = i;
                    break;
                }
            }
            // 如果列不存在，跳过此索引
            if (col_idx == columns_.size())
                continue;

            // 将列添加到已索引列集合
            indexed_columns_.insert(col);
            // 从存储加载索引行
            auto entries = storage_->load_index_rows(name_, col);
            // 获取或创建对应列的索引
            auto& idx = indexes_[col];
            idx.clear(); // 清空现有索引（如果有）
            // 将持久化的 (列值 → 主键) 条目载入内存索引（超集，查询时按可见性重查过滤）
            for (const auto& [val, pk]: entries) {
                idx.emplace(val, pk);
            }
        }

        // 加载索引名注册表（用于支持按名称 DROP INDEX）
        index_name_registry_ = storage_->load_index_registry(name_);
    }

    /**
     * @brief 向表中插入一行数据
     * @param row 要插入的行数据
     * @throw std::runtime_error 如果行宽度与表模式不匹配
     */
    void Table::insert(Row row) {
        if (row.values.size() != columns_.size()) {
            throw std::runtime_error("[Table] Row width does not match table schema");
        }
        if (storage_) {
            storage_->append_row(name_, columns_, row); // 持久化到存储
            index_row(row);                             // 增量维护索引（不经 rows_，去除全量常驻）
        } else {
            const std::size_t new_rid = rows_.size(); // 新行的ID
            rows_.emplace_back(std::move(row));       // 添加到内存中的行集合
            update_indexes_for_row(new_rid);          // 增量更新索引
        }
        bump_write_counter();
    }

    /**
     * @brief 批量插入多行数据
     * @param rows 要插入的行数据列表
     * @throw std::runtime_error 如果某行宽度与表模式不匹配
     */
    void Table::insert_batch(std::vector<Row> rows) {
        insert_batch(std::move(rows), 0);
    }

    void Table::insert_batch(std::vector<Row> rows, uint64_t commit_ts) {
        if (rows.empty())
            return;

        // 验证所有行的宽度
        for (const auto& row: rows) {
            if (row.values.size() != columns_.size()) {
                throw std::runtime_error("[Table] Row width does not match table schema");
            }
        }

        if (storage_) {
            // 持久化到存储 + 增量维护索引（不经 rows_，去除全量常驻）。
            storage_->append_rows(name_, columns_, rows, commit_ts);
            for (const auto& row: rows)
                index_row(row);
        } else {
            // 纯内存表：批量添加到 rows_。
            const std::size_t start_rid = rows_.size();
            for (auto& row: rows) {
                rows_.emplace_back(std::move(row));
            }
            for (std::size_t i = 0; i < rows.size(); ++i) {
                update_indexes_for_row(start_rid + i);
            }
        }
        bump_write_counter();
    }

    /**
     * @brief 重写所有表数据到存储引擎
     *
     * 该方法将内存中的所有表数据和索引写入到存储引擎中，用于数据持久化
     * @note 如果没有设置存储引擎（纯内存表），该方法将直接返回，不执行任何操作
     * @post 所有表数据和索引都被持久化到存储引擎中
     */
    void Table::rewrite_all() {
        if (!storage_)
            return;                                      // 如果没有存储引擎，直接返回
        // 从可见快照重写（去除 rows_ 依赖）。
        auto src = storage_->scan_visible(name_, columns_, UINT64_MAX);
        storage_->rewrite_table(name_, columns_, src); // 重写表数据
        // 全表重写后重建各列索引（value→pk）。
        for (const auto& col: indexed_columns_) {
            if (auto ci = find_column(col))
                rebuild_index_for_column(col, *ci);
        }
    }

    void Table::persist_row_upsert(const Row& row, uint64_t commit_ts) {
        if (!storage_)
            return;
        storage_->append_row(name_, columns_, row, commit_ts);
        index_row(row); // 增量维护二级索引（value→pk 超集）
        bump_write_counter();
    }

    void Table::persist_row_delete(const Value& key, uint64_t commit_ts) {
        if (!storage_)
            return;
        storage_->delete_row_by_key(name_, columns_, key, commit_ts);
        bump_write_counter();
    }

    std::vector<Row> Table::scan_visible(uint64_t snapshot_ts) const {
        if (!storage_)
            return rows_;
        return storage_->scan_visible(name_, columns_, snapshot_ts);
    }

    std::generator<Row> Table::scan_visible_stream(uint64_t snapshot_ts) const {
        if (!storage_) {
            for (const auto& r: rows_)
                co_yield r;
            co_return;
        }
        for (auto&& r: storage_->scan_visible_stream(name_, columns_, snapshot_ts))
            co_yield std::move(r);
    }

    std::optional<Row> Table::lookup_visible(const Value& pk, uint64_t snapshot_ts) const {
        if (!storage_) {
            for (const auto& r: rows_) {
                if (!r.values.empty() && ValueEq{}(r.values.front(), pk)) {
                    return r;
                }
            }
            return std::nullopt;
        }
        return storage_->lookup_visible(name_, columns_, pk, snapshot_ts);
    }

    void Table::refresh_indexes() {
        // 二级索引改为增量维护（写入时经 index_row 追加 value→pk 超集条目）。
        // 不再从 rows_（仅最新版本）全量重建，以免丢失历史值而破坏 MVCC 下的索引正确性。
    }

    /**
     * @brief 为指定列创建索引（显式指定索引名）
     * @param index_name 索引名称，如 `idx_emp_salary`
     * @param column     要索引的列名
     * @throw std::runtime_error 如果列不存在
     */
    void Table::create_index(const std::string& index_name, const std::string& column) {
        std::size_t col_idx = columns_.size();
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].name == column) {
                col_idx = i;
                break;
            }
        }
        if (col_idx == columns_.size())
            throw std::runtime_error("[Table] Unknown column: " + column);

        if (!indexed_columns_.count(column)) {
            // 索引尚不存在：创建文件并构建内存索引
            if (storage_)
                storage_->create_index_file(name_, column);
            indexed_columns_.insert(column);
            rebuild_index_for_column(column, col_idx);
        }

        // 无论索引是否刚创建，始终注册名称映射（幂等操作）
        if (!index_name.empty()) {
            index_name_registry_[index_name] = column;
            if (storage_)
                storage_->save_index_registry(name_, index_name_registry_);
        }
    }

    /**
     * @brief 为指定列创建索引（自动生成索引名 `idx_{table}_{column}`）
     * @param column 要索引的列名
     * @throw std::runtime_error 如果列不存在
     */
    void Table::create_index(const std::string& column) {
        create_index("idx_" + name_ + "_" + column, column);
    }

    /**
     * @brief 按索引名删除索引
     * @param index_name 创建时使用的索引名
     * @return true 如果索引被成功删除，false 如果索引不存在
     */
    bool Table::drop_index(const std::string& index_name) {
        // 先在注册表中查找对应的列名
        auto it = index_name_registry_.find(index_name);
        std::string column;
        if (it != index_name_registry_.end()) {
            column = it->second;
        } else {
            // 兼容旧数据：直接把 index_name 当 column 名尝试
            column = index_name;
        }

        if (indexed_columns_.count(column) == 0) {
            return false;
        }

        indexed_columns_.erase(column);
        indexes_.erase(column);
        index_name_registry_.erase(index_name);

        if (storage_) {
            storage_->drop_index(name_, column);
            storage_->save_index_registry(name_, index_name_registry_);
        }

        return true;
    }

    /**
     * @brief 检查指定列是否有索引
     * @param column 要检查的列名
     * @return 如果有索引返回true，否则返回false
     */
    bool Table::has_index(const std::string& column) const {
        return indexed_columns_.count(column) > 0; // 检查列是否在已索引集合中
    }

    /**
     * @brief 使用索引查找匹配的行ID
     * @param column 索引列名
     * @param key 要查找的值
     * @return 匹配的行ID列表
     */
    std::vector<Value> Table::lookup_index(const std::string& column, const Value& key) const {
        std::vector<Value> out;          // 结果：匹配的主键集合
        auto it = indexes_.find(column); // 查找索引
        if (it == indexes_.end())
            return out;                            // 索引不存在，返回空列表
        std::unordered_set<Value, ValueHash, ValueEq> seen;
        auto range = it->second.equal_range(key); // 查找匹配的键范围
        // 收集所有匹配的主键（超集索引可能含重复 pk，去重）
        for (auto iter = range.first; iter != range.second; ++iter) {
            if (seen.insert(iter->second).second)
                out.push_back(iter->second);
        }
        return out;
    }

    std::vector<Value> Table::lookup_index_range(const std::string& column, const std::optional<Value>& low,
                                                 bool low_inclusive, const std::optional<Value>& high,
                                                 bool high_inclusive) const {
        std::vector<Value> out;
        auto it = indexes_.find(column);
        if (it == indexes_.end())
            return out;
        const auto& m = it->second;
        // 下界：>=low 用 lower_bound；>low 用 upper_bound。
        auto begin = low.has_value() ? (low_inclusive ? m.lower_bound(*low) : m.upper_bound(*low)) : m.begin();
        // 上界：<=high 用 upper_bound（含 high）；<high 用 lower_bound（不含 high）。
        auto end = high.has_value() ? (high_inclusive ? m.upper_bound(*high) : m.lower_bound(*high)) : m.end();
        std::unordered_set<Value, ValueHash, ValueEq> seen;
        for (auto iter = begin; iter != end; ++iter) {
            if (seen.insert(iter->second).second)
                out.push_back(iter->second);
        }
        return out;
    }

    /**
     * @brief 为单行增量维护所有二级索引（value→pk 超集条目）。
     * @param rid rows_ 中的行下标（仅用于取行数据）。
     */
    void Table::update_indexes_for_row(std::size_t rid) {
        if (rid >= rows_.size())
            return;
        index_row(rows_[rid]);
    }

    /**
     * @brief 为给定行的每个已索引列追加 (列值 → 主键) 条目（内存 + 持久化）。
     *
     * 采用“超集 + 可见性重查”策略：不删除旧值条目，查询时用 lookup_visible 重查过滤，
     * 从而在 MVCC（含索引列被更新）下保持正确。
     */
    void Table::index_row(const Row& row) {
        if (indexed_columns_.empty())
            return;
        if (row.values.empty())
            return; // 无主键列
        const Value& pk = row.values.front();
        for (const auto& col_name: indexed_columns_) {
            auto ci = find_column(col_name);
            if (!ci || *ci >= row.values.size())
                continue;
            const Value& v = row.values[*ci];
            indexes_[col_name].emplace(v, pk);
            if (storage_)
                storage_->append_index_entry(name_, col_name, v, pk);
        }
    }

    /**
     * @brief 为指定列重建索引
     * @param column 列名
     * @param col_idx 列索引
     */
    void Table::rebuild_index_for_column(const std::string& column, std::size_t col_idx) {
        auto& idx = indexes_[column];
        idx.clear();
        // 数据来源：存储型从可见快照重建（去除 rows_ 依赖），纯内存表读 rows_。
        std::vector<Row> src = storage_ ? storage_->scan_visible(name_, columns_, UINT64_MAX) : rows_;
        std::vector<std::pair<Value, Value>> entries;
        entries.reserve(src.size());

        for (const auto& row: src) {
            if (row.values.empty() || col_idx >= row.values.size())
                continue;
            const Value& pk = row.values.front();
            idx.emplace(row.values[col_idx], pk);
            entries.emplace_back(row.values[col_idx], pk);
        }

        if (storage_) {
            storage_->write_index_rows(name_, column, entries);
        }
    }

    std::optional<std::size_t> Table::find_column(const std::string& column_name) const {
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].name == column_name)
                return i;
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------------------
    // Catalog
    // ---------------------------------------------------------------------------

    void Catalog::register_table(std::shared_ptr<Table> table) {
        if (table) {
            tables_by_name_[table->name()] = table;
            tables_by_oid_[table->oid()] = table;
        }
    }

    bool Catalog::unregister_table(const std::string& name) {
        auto it = tables_by_name_.find(name);
        if (it != tables_by_name_.end()) {
            tables_by_oid_.erase(it->second->oid());
            tables_by_name_.erase(it);
            return true;
        }
        return false;
    }

    std::shared_ptr<Table> Catalog::lookup(const std::string& name) const {
        auto it = tables_by_name_.find(name);
        return it != tables_by_name_.end() ? it->second : nullptr;
    }

    std::shared_ptr<Table> Catalog::lookup(Oid oid) const {
        auto it = tables_by_oid_.find(oid);
        return it != tables_by_oid_.end() ? it->second : nullptr;
    }

    bool Catalog::contains(const std::string& name) const {
        return tables_by_name_.count(name) > 0;
    }

    std::size_t Catalog::size() const noexcept {
        return tables_by_name_.size();
    }

    bool Catalog::empty() const noexcept {
        return tables_by_name_.empty();
    }

    std::vector<std::string> Catalog::table_names() const {
        std::vector<std::string> names;
        names.reserve(tables_by_name_.size());
        for (const auto& [name, _]: tables_by_name_) {
            names.push_back(name);
        }
        return names;
    }

    void Catalog::clear() {
        tables_by_name_.clear();
        tables_by_oid_.clear();
    }

} // namespace corodb
