// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file lsm_storage_engine.h @brief LSM 存储引擎定义。 */

#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "corodb/storage/buffer_pool.h"
#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/storage_engine_common.h"
#include "corodb/threading/thread_pool.h"

namespace corodb {

    /** @brief 基于 WAL + MemTable + SSTable 层级的存储引擎。 */
    class LSMTreeEngine : public StorageEngine {
    public:
        /** @brief 创建 LSM 存储引擎实例。 */
        explicit LSMTreeEngine(std::string base_dir, std::size_t buffer_pages = 256);

        /** @brief 注入压缩时可见性 GC 的安全水位回调。 */
        void set_gc_horizon(std::function<uint64_t()> fn) {
            gc_horizon_fn_ = std::move(fn);
        }

        /** @brief 设置后台 compaction 使用的线程池。 */
        void set_thread_pool(ThreadPool* pool) {
            pool_ = pool;
        }

        /**
         * @brief 检查表是否存在（检查 WAL 或任意 SSTable 层文件）。
         */
        [[nodiscard]] bool table_exists(const std::string& name) const override;

        /**
         * @brief 创建空的 L0 SSTable 和 WAL 文件。
         * @throws std::runtime_error 若写入失败。
         */
        void create_table(const std::string& name, const std::vector<Column>& columns) override;

        /**
         * @brief 从 SSTable 头部读取表模式。
         * @throws std::runtime_error 若表不存在或文件损坏。
         */
        [[nodiscard]] std::vector<Column> load_schema(const std::string& name) const override;

        /**
         * @brief 合并所有 SSTable 层级与 MemTable，返回全量行数据。
         * @throws std::runtime_error 若表不存在。
         */
        [[nodiscard]] std::vector<Row> load_rows(const std::string& name, const std::vector<Column>& columns) override;

        /**
         * @brief 先写 WAL，再更新 MemTable（达到阈值时自动刷写）。
         * @throws std::runtime_error 若 WAL 写入失败。
         */
        void append_row(const std::string& name, const std::vector<Column>& columns, const Row& row,
                        uint64_t commit_ts = 0) override;

        /**
         * @brief 清除所有现有数据并写入新行集合（用于 UPDATE/DELETE 后的全量覆写）。
         * @throws std::runtime_error 若重写失败。
         */
        void rewrite_table(const std::string& name, const std::vector<Column>& columns,
                           const std::vector<Row>& rows) override;

        /** @brief 以 tombstone 形式按主键删除一行。 */
        void delete_row_by_key(const std::string& name, const std::vector<Column>& columns, const Value& key,
                               uint64_t commit_ts = 0) override;

        /** @brief 扫描磁盘文件并返回当前可见的最大 commit_ts。 */
        [[nodiscard]] uint64_t max_observed_commit_ts() const override;

        /** @brief 点查 snapshot_ts 时刻可见的主键版本。 */
        [[nodiscard]] std::optional<Row> lookup_visible(const std::string& name, const std::vector<Column>& columns,
                                                        const Value& pk, uint64_t snapshot_ts) override;

        /** @brief 扫描 snapshot_ts 时刻整表可见的数据。 */
        [[nodiscard]] std::vector<Row> scan_visible(const std::string& name, const std::vector<Column>& columns,
                                                    uint64_t snapshot_ts) override;

        /** @brief MVCC 流式扫描：对 memtable 快照 + 各 SSTable 源做 k 路归并，逐行惰性 yield。 */
        [[nodiscard]] std::generator<Row> scan_visible_stream(const std::string& name,
                                                              const std::vector<Column>& columns,
                                                              uint64_t snapshot_ts) override;

        /** @brief 列出表的所有索引列名。 */
        [[nodiscard]] std::vector<std::string> list_indexes(const std::string& table) const override;

        /** @brief 删除表及其所有关联文件，返回是否成功删除。 */
        bool drop_table(const std::string& name) override;

        /** @brief 删除指定列的索引文件，返回是否成功删除。 */
        bool drop_index(const std::string& table, const std::string& column) override;

        /**
         * @brief 为指定列创建索引文件（空结构，之后通过 write_index_rows 填充）。
         * @throws std::runtime_error 若创建失败。
         */
        void create_index_file(const std::string& table, const std::string& column) override;

        /** @brief 批量写入索引条目。 */
        void write_index_rows(const std::string& table, const std::string& column,
                              const std::vector<std::pair<Value, Value>>& entries) override;

        /** @brief 增量追加单个索引条目（value→pk），追加一个单条 chunk。 */
        void append_index_entry(const std::string& table, const std::string& column, const Value& value,
                                const Value& pk) override;

        /** @brief 加载全部索引条目。 */
        [[nodiscard]] std::vector<std::pair<Value, Value>>
        load_index_rows(const std::string& table, const std::string& column) const override;

        /** @brief 持久化索引名注册表（index_name → column）。 */
        void save_index_registry(const std::string& table,
                                 const std::unordered_map<std::string, std::string>& registry) override;

        /** @brief 加载索引名注册表。 */
        [[nodiscard]] std::unordered_map<std::string, std::string>
        load_index_registry(const std::string& table) const override;

        /** @brief 强制刷盘所有表并截断 WAL（用于 CHECKPOINT / 备份）。 */
        void checkpoint() override;

        /** @brief 将 commit_ts 写入全局提交日志（跨表原子提交点，崩溃原子恢复）。 */
        void mark_committed(uint64_t commit_ts) override;

    private:
        std::string base_dir_;             ///< 存储基础目录
        std::size_t memtable_limit_bytes_; ///< MemTable 大小阈值（构造时取自 Config）
        mutable BufferPool bufpool_;       ///< 缓冲池，用于管理SSTable页面缓存

        /** @brief WAL 记录。 */
        struct WalRecord {
            uint8_t type;
            std::string payload;
        };

        /** @brief MemTable 中的单个版本条目。 */
        struct MemEntry {
            Value pk{};
            Row row;
            bool tombstone{ false };
            uint64_t commit_ts{ 0 };
        };

        /** @brief MemTable 中用于排序版本的复合键。 */
        struct MVCCKey {
            Value pk{};
            uint64_t commit_ts{ 0 };
        };

        struct MVCCKeyCompare {
            using is_transparent = void;
            bool operator()(const MVCCKey& a, const MVCCKey& b) const noexcept {
                ValueLess lt;
                if (lt(a.pk, b.pk))
                    return true;
                if (lt(b.pk, a.pk))
                    return false;
                return a.commit_ts > b.commit_ts; // newer first within same pk
            }
        };

        /** @brief 某张表的内存态缓存。 */
        struct TableState {
            std::vector<Column> schema;
            std::map<MVCCKey, MemEntry, MVCCKeyCompare> memtable;
            std::size_t memtable_bytes{ 0 };
            mutable std::shared_mutex mutex;

            /// 防止同一表多次 flush 触发的并发 Compaction。
            /// try_lock 静默失败——已有 Compaction 运行时，新 flush 的 Compaction 跳过。
            std::mutex compaction_mutex;

            // 扫描结果缓存：每次写入通过 write_version 失效。
            std::atomic<uint64_t> write_version{ 0 };
            mutable std::mutex cache_rebuild_mutex; ///< 序列化缓存重建，防止惊群效应
            mutable std::shared_mutex scan_cache_mutex;
            mutable std::vector<Row> scan_cache;
            mutable uint64_t scan_cache_version{ ~uint64_t{ 0 } };
            mutable uint64_t scan_cache_ts{ 0 };
            mutable uint64_t scan_cache_max_commit_ts{ 0 }; ///< 重建时观察到的最大 commit_ts
            mutable bool scan_cache_valid{ false };
        };

        mutable std::unordered_map<std::string, std::unique_ptr<TableState>> states_;
        mutable std::shared_mutex states_mutex_;

        /** @brief 获取或创建某张表的缓存状态。 */
        TableState* get_or_create_state(const std::string& name) const;

        [[nodiscard]] std::string base_path(const std::string& name) const;

        [[nodiscard]] std::string wal_path(const std::string& name) const;

        [[nodiscard]] std::string level_path(const std::string& name, int level) const;

        void ensure_base_dir() const;

        /// 按需从 SSTable 与 WAL 恢复表状态。
        void load_state_if_needed(const std::string& name, const std::vector<Column>& columns) const;

        /// 将 MemTable 合并到 L0 SSTable 并触发层级压缩。
        void flush_memtable(const std::string& name, const std::vector<Column>& columns);

        /// 读取 SSTable 文件，返回含 commit_ts 的条目列表。
        /// 内部按 (路径, mtime) 缓存解码结果，避免重复反序列化；返回 shared_ptr 以零拷贝共享。
        [[nodiscard]] std::shared_ptr<const std::vector<MemEntry>>
        read_sstable(const std::string& path, const std::vector<Column>& columns) const;

        /// 以 commit_ts 感知格式写入 SSTable 文件。
        void write_sstable(const std::string& path, const std::vector<Column>& columns,
                           const std::vector<MemEntry>& entries) const;

        /// 删除表的 WAL、全部 SSTable 层级及索引文件。
        void drop_table_files(const std::string& name);

        /** @brief 执行层级压缩。 */
        void compact_levels(const std::string& name, const std::vector<Column>& columns);

    private:
        // Internal helpers
        // flush_memtable_internal 接受一个 memtable 快照，在不持有 state->mutex 的情况下执行 I/O
        void flush_memtable_internal(const std::string& name, const std::vector<Column>& columns,
                                     std::map<MVCCKey, MemEntry, MVCCKeyCompare> mem_snapshot);
        void load_state_locked(TableState* state, const std::string& name, const std::vector<Column>& columns) const;

        ThreadPool* pool_{ nullptr };
        std::function<uint64_t()> gc_horizon_fn_;

        /// SSTable 反序列化缓存：避免重复 decode_row 的 O(N) 开销。
        /// Key = 绝对路径；Value = (mtime_ns, decoded entries shared_ptr)。
        /// SSTable 文件不可变（compaction/flush 后写新文件），mtime 变化即缓存失效。
        /// 缓存上限由 Config::lsm.sst_cache_entries 控制，默认 256。
        mutable std::shared_mutex sst_decoded_cache_mutex_;
        mutable std::unordered_map<std::string, std::pair<int64_t, std::shared_ptr<const std::vector<MemEntry>>>>
                sst_decoded_cache_;
        mutable std::list<std::string> sst_cache_lru_; ///< LRU 顺序：前 = LRU，后 = MRU

        /// 页脚缓存：键 = 绝对路径，值 = (mtime, footer)。
        mutable std::shared_mutex sst_footer_cache_mutex_;
        mutable std::unordered_map<std::string, std::pair<int64_t, storage_internal::SstFooter>> sst_footer_cache_;

        // ---- 全局提交日志（跨表原子提交） ----
        /// 单一的全局提交日志文件，记录已提交的 commit_ts（跨所有表）。
        [[nodiscard]] std::string commit_log_path() const;
        /// 惰性读入已提交 commit_ts 集合（供崩溃恢复判定行记录是否属于已提交事务）。
        void ensure_committed_loaded() const;
        mutable std::mutex commit_log_mutex_;
        mutable std::unordered_set<uint64_t> committed_ts_; ///< 已提交 commit_ts 缓存
        mutable bool committed_loaded_{ false };            ///< 提交集合是否已从磁盘加载
        mutable bool commit_log_present_{ false };          ///< 提交日志文件是否存在（不存在则旧格式全量回放）
    };

} // namespace corodb
