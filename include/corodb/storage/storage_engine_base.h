// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file storage_engine_base.h @brief 存储引擎基类定义。 */

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "corodb/storage/table.h"

namespace corodb {

    /**
     * @brief 可插拔存储引擎抽象接口。
     *
     * 定义所有存储引擎实现（如 LSMTreeEngine）必须满足的核心接口。
     */
    class StorageEngine {
    public:
        /// 虚析构函数，确保通过基类指针正确析构派生类。
        virtual ~StorageEngine() = default;

        // 禁止复制和移动
        StorageEngine(const StorageEngine&) = delete;
        StorageEngine& operator=(const StorageEngine&) = delete;
        StorageEngine(StorageEngine&&) = delete;
        StorageEngine& operator=(StorageEngine&&) = delete;

        /**
         * @name 表管理操作
         * @{
         */

        /**
         * @brief 检查指定表是否存在于存储引擎中。
         */
        [[nodiscard]] virtual bool table_exists(const std::string& name) const = 0;

        /**
         * @brief 创建新表。
         * @throws std::runtime_error 若表已存在或无法创建文件。
         */
        virtual void create_table(const std::string& name, const std::vector<Column>& columns) = 0;

        /**
         * @brief 加载表的模式（列定义）。
         * @throws std::runtime_error 若表不存在或模式数据损坏。
         */
        [[nodiscard]] virtual std::vector<Column> load_schema(const std::string& name) const = 0;

        /** @} */

        /**
         * @name 数据操作
         * @{
         */

        /**
         * @brief 加载表的所有行数据。
         * @throws std::runtime_error 若表不存在或数据损坏。
         */
        [[nodiscard]] virtual std::vector<Row> load_rows(const std::string& name,
                                                         const std::vector<Column>& columns) = 0;

        /**
         * @brief 向表中追加一行数据。
         * @throws std::runtime_error 若表不存在或写入失败。
         */
        virtual void append_row(const std::string& name, const std::vector<Column>& columns, const Row& row,
                                uint64_t commit_ts = 0) = 0;

        /**
         * @brief 批量向表中追加多行数据。
         * @note 默认实现逐行调用 append_row，子类可覆盖以提供更高效的实现。
         */
        virtual void append_rows(const std::string& name, const std::vector<Column>& columns,
                                 const std::vector<Row>& rows, uint64_t commit_ts = 0);

        /**
         * @brief 重写整个表的内容（破坏性操作）。
         * @throws std::runtime_error 若表不存在或重写失败。
         */
        virtual void rewrite_table(const std::string& name, const std::vector<Column>& columns,
                                   const std::vector<Row>& rows) = 0;

        /**
         * @brief 按主键删除一行（增量删除，避免 rewrite_table 全表重写）
         *
         * 默认实现 fallback 到 rewrite_table；LSM 引擎应该覆盖为写入 tombstone。
         *
         * @param name 表名
         * @param columns 列定义（用于 fallback / 加载状态）
         * @param key   要删除的主键
         */
        virtual void delete_row_by_key(const std::string& name, const std::vector<Column>& columns, int64_t key,
                                       uint64_t commit_ts = 0);

        /**
         * @brief 扫描磁盘上所有 WAL/SSTable，返回观察到的最大 commit_ts。
         *
         * 用于启动恢复：让 TransactionManager.next_ts_ bootstrap 到 max+1，
         * 避免重启后重新分配已存在的 commit_ts，破坏 MVCC 单调性。
         * 默认实现返回 0（无 commit_ts 持久化的引擎）。
         */
        [[nodiscard]] virtual uint64_t max_observed_commit_ts() const;

        /**
         * @brief MVCC 快照读：获取某 pk 在 snapshot_ts 时刻可见的行。
         *
         * 默认实现退化为 load_rows + 线性查找最新版本（不使用 snapshot_ts）。
         * 支持多版本的引擎（LSM）应 override。
         */
        [[nodiscard]] virtual std::optional<Row>
        lookup_visible(const std::string& name, const std::vector<Column>& columns, int64_t pk, uint64_t snapshot_ts);

        /**
         * @brief MVCC 全表扫描：返回 snapshot_ts 时刻每个 pk 可见的最新版本。
         *
         * 默认实现退化为 load_rows（最新已提交快照）。LSM 会 override。
         */
        [[nodiscard]] virtual std::vector<Row> scan_visible(const std::string& name, const std::vector<Column>& columns,
                                                            uint64_t snapshot_ts);

        /** @} */

        /**
         * @name 索引操作
         * @{
         */

        /**
         * @brief 列出表的所有索引。
         * @throws std::runtime_error 若表不存在。
         */
        [[nodiscard]] virtual std::vector<std::string> list_indexes(const std::string& table) const = 0;

        /**
         * @brief 删除表及其所有关联文件。
         */
        virtual bool drop_table(const std::string& name) = 0;

        /**
         * @brief 删除指定列的索引文件。
         */
        virtual bool drop_index(const std::string& table, const std::string& column) = 0;

        /**
         * @brief 为指定列创建索引文件。
         * @throws std::runtime_error 若表/列不存在或索引已存在。
         */
        virtual void create_index_file(const std::string& table, const std::string& column) = 0;

        /**
         * @brief 批量写入索引条目。
         * @throws std::runtime_error 若索引文件不存在或写入失败。
         */
        virtual void write_index_rows(const std::string& table, const std::string& column,
                                      const std::vector<std::pair<Value, std::size_t>>& entries) = 0;

        /**
         * @brief 追加单个索引条目（增量更新）。
         * @note 默认实现通过重写全量索引实现，子类可覆盖以提供增量追加。
         */
        virtual void append_index_entry(const std::string& table, const std::string& column, const Value& value,
                                        std::size_t rid);

        /**
         * @brief 加载所有索引条目。
         * @throws std::runtime_error 若索引不存在或数据损坏。
         */
        [[nodiscard]] virtual std::vector<std::pair<Value, std::size_t>>
        load_index_rows(const std::string& table, const std::string& column) const = 0;

        /**
         * @brief 持久化索引名注册表（index_name → column）。
         * @note 默认实现为空操作，支持持久化的引擎应覆盖此方法。
         */
        virtual void save_index_registry(const std::string& table,
                                         const std::unordered_map<std::string, std::string>& registry);

        /**
         * @brief 加载索引名注册表。
         * @return index_name → column 的映射，若无持久化数据则返回空 map。
         */
        [[nodiscard]] virtual std::unordered_map<std::string, std::string>
        load_index_registry(const std::string& table) const;

        /** @} */

        /**
         * @name 事务操作
         * @{
         */

        /**
         * @brief 开始新事务（占位接口，事务管理已由上层 Session 负责）。
         * @note 当前版本始终返回 0；保留此接口供未来存储层 MVCC 扩展使用。
         */
        virtual uint64_t begin_transaction();

        /**
         * @brief 提交事务（占位接口）。
         * @note 默认实现始终返回 true。
         */
        virtual bool commit_transaction(uint64_t txn_id);

        /**
         * @brief 回滚事务（占位接口）。
         * @note 默认实现始终返回 true。
         */
        virtual bool rollback_transaction(uint64_t txn_id);

        /**
         * @brief 在事务中追加行（占位接口）。
         * @note 默认实现直接调用 append_row。
         */
        virtual void append_row_in_transaction(uint64_t txn_id, const std::string& name,
                                               const std::vector<Column>& columns, const Row& row);

        /**
         * @brief 检查是否支持事务。
         */
        [[nodiscard]] virtual bool supports_transactions() const noexcept;

        /** @brief 强制刷盘、压缩并截断 WAL（用于备份/快照）。 */
        virtual void checkpoint() = 0;

        /** @} */

    protected:
        /// 仅供派生类使用的默认构造函数。
        StorageEngine() = default;
    };

} // namespace corodb
