// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file storage_engine_base.cpp
// @brief StorageEngine 抽象基类默认实现（非事务型引擎的退化实现）。

#include "corodb/storage/storage_engine_base.h"

#include "corodb/storage/storage_engine_common.h"

namespace corodb {

    /**
     * @brief 批量追加多行数据（逐行调用 append_row）。
     */
    void StorageEngine::append_rows(const std::string& name, const std::vector<Column>& columns,
                                    const std::vector<Row>& rows, uint64_t commit_ts) {
        for (const auto& row: rows) {
            append_row(name, columns, row, commit_ts);
        }
    }

    /**
     * @brief 按主键删除行（默认为空操作；不支持增量删除的引擎可忽略此调用）。
     */
    void StorageEngine::delete_row_by_key(const std::string& name, const std::vector<Column>& columns,
                                          const Value& key, uint64_t commit_ts) {
        // 默认实现为空操作；不支持增量删除的引擎可忽略此调用
        (void)name;
        (void)columns;
        (void)key;
        (void)commit_ts;
    }

    /**
     * @brief 返回存储引擎已见到的最大提交时间戳（默认返回 0）。
     */
    uint64_t StorageEngine::max_observed_commit_ts() const {
        return 0;
    }

    /**
     * @brief 粗略估算表当前可见行数（默认退化实现：全量加载计数）。
     */
    std::size_t StorageEngine::estimate_row_count(const std::string& name, const std::vector<Column>& columns) {
        return load_rows(name, columns).size();
    }

    /**
     * @brief 按主键点查可见行（默认退化为全表扫描，不感知 snapshot_ts）。
     */
    std::optional<Row> StorageEngine::lookup_visible(const std::string& name, const std::vector<Column>& columns,
                                                     const Value& pk, uint64_t /*snapshot_ts*/) {
        // 默认退化为全表扫描，不感知 snapshot_ts
        for (const auto& r: load_rows(name, columns)) {
            if (!r.values.empty() && ValueEq{}(storage_internal::extract_key(r, columns), pk)) {
                return r;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 全表扫描返回可见行（默认委托 load_rows，不感知 snapshot_ts）。
     */
    std::vector<Row> StorageEngine::scan_visible(const std::string& name, const std::vector<Column>& columns,
                                                 uint64_t /*snapshot_ts*/) {
        return load_rows(name, columns);
    }

    /**
     * @brief 流式扫描默认实现：基于 scan_visible 物化后逐行 yield（不支持真流式的引擎的退化路径）。
     */
    std::generator<Row> StorageEngine::scan_visible_stream(const std::string& name, const std::vector<Column>& columns,
                                                           uint64_t snapshot_ts) {
        for (auto& r: scan_visible(name, columns, snapshot_ts))
            co_yield std::move(r);
    }

    /**
     * @brief 追加索引条目（默认实现：全量重写；子类可覆盖为增量追加）。
     */
    void StorageEngine::append_index_entry(const std::string& table, const std::string& column, const Value& value,
                                           const Value& pk) {
        // 默认实现：加载现有条目，追加后全量重写；子类可覆盖为增量追加
        auto entries = load_index_rows(table, column);
        entries.emplace_back(value, pk);
        write_index_rows(table, column, entries);
    }

    /**
     * @brief 持久化索引名注册表（默认空操作）。
     */
    void StorageEngine::save_index_registry(const std::string& /*table*/,
                                            const std::unordered_map<std::string, std::string>& /*registry*/) {
        // 默认空操作；不支持持久化的引擎忽略此调用
    }

    /**
     * @brief 加载索引名注册表（默认返回空 map）。
     */
    std::unordered_map<std::string, std::string>
    StorageEngine::load_index_registry(const std::string& /*table*/) const {
        return {};
    }

    /**
     * @brief 开启事务并返回事务 ID（默认返回 0，非事务型引擎使用）。
     */
    uint64_t StorageEngine::begin_transaction() {
        return 0;
    }

    /**
     * @brief 提交事务（默认返回 true，非事务型引擎使用）。
     */
    bool StorageEngine::commit_transaction(uint64_t /*txn_id*/) {
        return true;
    }

    /**
     * @brief 回滚事务（默认返回 true，非事务型引擎使用）。
     */
    bool StorageEngine::rollback_transaction(uint64_t /*txn_id*/) {
        return true;
    }

    /**
     * @brief 在事务上下文中追加行（默认忽略 txn_id，直接调用 append_row）。
     */
    void StorageEngine::append_row_in_transaction(uint64_t txn_id, const std::string& name,
                                                  const std::vector<Column>& columns, const Row& row) {
        (void)txn_id;
        append_row(name, columns, row);
    }

    /**
     * @brief 返回引擎是否支持事务（默认返回 false）。
     */
    bool StorageEngine::supports_transactions() const noexcept {
        return false;
    }

    /**
     * @brief 将 commit_ts 标记为已提交（默认空操作；无 WAL 的引擎无需原子恢复）。
     */
    void StorageEngine::mark_committed(uint64_t /*commit_ts*/) {
        // 默认空操作；崩溃原子恢复由支持 WAL 的引擎（LSMTreeEngine）覆盖实现。
    }

} // namespace corodb
