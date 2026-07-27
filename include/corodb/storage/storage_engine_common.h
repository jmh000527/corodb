// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file storage_engine_common.h
 * @brief 存储引擎公共数据结构与编码格式。
 *
 * ============================================================================
 *                          CoroDB 存储引擎类型概览
 * ============================================================================
 *
 *   +-----------------------------------------------------------------+
 *   |                    CoroDB Storage Engine Layer                  |
 *   +-----------------------------------------------------------------+
 *   |                       LSMTreeEngine                             |
 *   |                    LSM-Tree Storage                             |
 *   +-----------------------------------------------------------------+
 *   | Magic: 0x4C534D32                                              |
 *   +-----------------------------------------------------------------+
 *   | MemTable + SSTable  |  O(1) write  |  Background Compaction    |
 *   +-----------------------------------------------------------------+
 *                                   |
 *                                   v
 *   +-----------------------------------------------------------------+
 *   |                      Common Infrastructure                      |
 *   +---------------------+---------------------+---------------------+
 *   |   Buffer Pool       |   WAL (Write-Ahead  |   Schema Serialize  |
 *   |   Page Cache        |   Log) Recovery     |   Row Encoding      |
 *   +---------------------+---------------------+---------------------+
 *
 * ============================================================================
 *                          通用页面结构 (PageHeader)
 * ============================================================================
 *
 *   Page Header (18 bytes):
 *   +----------+----------+------------+------------+----------+
 *   |   LSN    | Checksum | Slot_Count | Free_Start | Free_End |
 *   | (8 bytes)| (4 bytes)|  (2 bytes) |  (2 bytes) | (2 bytes)|
 *   +----------+----------+------------+------------+----------+
 *
 *   校验和算法: FNV-1a (32位)
 *   - 初始值: 2166136261
 *   - 素数: 16777619
 *
 * ============================================================================
 *                          WAL 记录格式
 * ============================================================================
 *
 *   WAL File Header (4 bytes):
 *   +----------------------------------------+
 *   | Magic: 0x57414C31 ("WAL1")             |
 *   +----------------------------------------+
 *
 *   WAL Record Format (9 + len bytes):
 *   +----------+----------+----------+-------------------------------+
 *   | Type     | Length   | Checksum | Payload                       |
 *   | (1 byte) | (4 bytes)| (4 bytes)| (variable)                    |
 *   +----------+----------+----------+-------------------------------+
 *
 *   Type 类型:
 *   - 0x01: INSERT 插入操作
 *   - 0x02: DELETE 删除操作
 *   - 0x03: BEGIN 事务开始
 *   - 0x04: COMMIT 事务提交
 *   - 0x05: ROLLBACK 事务回滚
 *
 * ============================================================================
 *                          Schema 序列化格式
 * ============================================================================
 *
 *   Table Schema Encoding:
 *   +----------------+------------------------------------------------+
 *   | col_count (4)  | Column 1 | Column 2 | ... | Column N           |
 *   +----------------+------------------------------------------------+
 *
 *   Single Column Encoding:
 *   +----------------+----------------------------+-------------------+
 *   | name_len (2)   | name (variable)            | type (1)          |
 *   | Column name len| Column name string         | Type identifier   |
 *   +----------------+----------------------------+-------------------+
 *
 *   类型标识 (type)：
 *   - 0x00: NULL
 *   - 0x01: Int64
 *   - 0x02: Text
 *
 * ============================================================================
 *                          行数据编码格式 (Row)
 * ============================================================================
 *
 *   Row Encoding:
 *   +------------------+---------------------------------------------+
 *   | value_count (4)  | Value 1 | Value 2 | ... | Value N           |
 *   | Number of values |                                             |
 *   +------------------+---------------------------------------------+
 *
 *   Value Encoding:
 *   +----------+-----------------------------------------------------+
 *   | Tag (1)  | Data                                                |
 *   +----------+-----------------------------------------------------+
 *   | 0x00     | (no data) - NULL value                              |
 *   +----------+-----------------------------------------------------+
 *   | 0x01     | +-------------------------------------------------+ |
 *   |          | | int64_t value (8 bytes, little-endian)         | |
 *   |          | +-------------------------------------------------+ |
 *   +----------+-----------------------------------------------------+
 *   | 0x02     | +--------------+----------------------------------+ |
 *   |          | | length (4)   | string data (variable, UTF-8)   | |
 *   |          | +--------------+----------------------------------+ |
 *   +----------+-----------------------------------------------------+
 *
 *   Example - Encoding row (1, "hello", NULL):
 *   +-----+-----+-----------------+-----+-----+-----------+-----+
 *   | 0x03| 0x01| 01 00 00 00 ... | 0x02| 0x05| h e l l o | 0x00|
 *   |count| tag | int64 value     | tag | len | string    | tag |
 *   +-----+-----+-----------------+-----+-----+-----------+-----+
 *
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "corodb/common/config.h"
#include "corodb/common/types.h"
#include "corodb/storage/buffer_pool.h"
#include "corodb/storage/table.h"

namespace corodb::storage_internal {

    // ---- 文件格式魔术数字 ----
    constexpr uint32_t kMagic = 0x43444232;      ///< "CDB2"
    constexpr uint32_t kWalMagic = 0x57414c31;   ///< "WAL1"
    constexpr uint32_t kLsmMagic = 0x4c534d32;   ///< LSM SSTable magic number
    constexpr uint32_t kIndexMagic = 0x53494458; ///< "SIDX"
    constexpr uint32_t kFooterMagic = 0x46543031; ///< "FT01"

    /// 全局提交日志（commit log）中的提交记录类型。payload = 8 字节 commit_ts（小端）。
    /// 提交时将 commit_ts 写入独立的全局提交日志作为跨表原子提交点；崩溃恢复时，
    /// 各表 WAL 中带 commit_ts 的行记录仅当其 commit_ts 出现在提交日志中才回放，
    /// 否则视为未完成提交的"撕裂"写入而丢弃，从而保证（含跨表）提交的原子性。
    constexpr uint8_t kWalCommitBarrier = 14;

    /** @brief SSTable 点查优化的布隆过滤器（~2% 假阳性率）。 */
    class BloomFilter {
    public:
        BloomFilter() = default;

        /** @brief 从编码后的键字节串列表构建布隆过滤器。 */
        void build(const std::vector<std::string>& keys);

        /** @brief 测试编码键是否可能存在。 */
        [[nodiscard]] bool maybe_contains(const std::string& key) const noexcept;

        /** @brief 过滤器是否已填充。 */
        [[nodiscard]] bool valid() const noexcept { return !bits_.empty(); }

        /** @brief 序列化为二进制。 */
        [[nodiscard]] std::string serialize() const;

        /** @brief 从二进制反序列化，成功返回 true。 */
        bool deserialize(const std::string& data);

    private:
        [[nodiscard]] std::pair<uint64_t, uint64_t> hash(const std::string& key) const noexcept;

        uint32_t size_{ 0 };
        uint8_t num_hashes_{ 3 };
        std::vector<uint8_t> bits_;
    };

    /** @brief SSTable 页脚（存储在数据页之后）。仅保留 Bloom（主键泛化后不再用 int64 min/max 裁剪）。 */
    struct SstFooter {
        BloomFilter bloom;

        [[nodiscard]] bool valid() const noexcept { return bloom.valid(); }
        [[nodiscard]] std::string serialize() const;
        bool deserialize(const std::string& data);
    };

    /** @brief 对齐到指定边界。 */
    [[nodiscard]] constexpr std::size_t align_up(std::size_t n, std::size_t align) noexcept {
        return (n + align - 1) / align * align;
    }

    /** @brief 文件头结构。 */
    struct FileHeader {
        uint32_t magic{ kMagic };
        uint32_t page_size{ static_cast<uint32_t>(Config::kDefaultPageSize) };
        uint32_t schema_bytes{ 0 };
    };

    /** @brief WAL 记录。 */
    struct WalRecord {
        uint8_t type{ 0 };
        std::string payload;
    };

    // ---- 编码工具 ----
    [[nodiscard]] uint32_t compute_payload_checksum(const std::string& payload);
    [[nodiscard]] uint8_t type_to_wire(TypeKind t) noexcept;
    [[nodiscard]] TypeKind wire_to_type(uint8_t w) noexcept;
    [[nodiscard]] uint32_t compute_page_checksum(const std::vector<char>& page);
    void stamp_page_checksum(std::vector<char>& page);
    void fsync_path(const std::filesystem::path& p);

    // ---- WAL 写入器 ----
    /** @brief WAL 同步模式。 */
    enum class WalSyncMode : uint8_t {
        Fast = 0,    ///< 仅 file_.flush()（OS 缓存，快速但断电丢失）
        Durable = 1, ///< 内核级 fsync（ACID 持久性）
    };

    /** @brief 设置全局 WAL 同步模式。默认：Fast。 */
    void set_wal_sync_mode(WalSyncMode mode) noexcept;
    [[nodiscard]] WalSyncMode get_wal_sync_mode() noexcept;

    /** @brief 持久化 WAL 文件句柄，支持 Group Commit。 */
    class WalWriter {
    public:
        explicit WalWriter(const std::filesystem::path& path);
        ~WalWriter();

        WalWriter(const WalWriter&) = delete;
        WalWriter& operator=(const WalWriter&) = delete;

        void append(uint8_t type, const std::string& payload);
        void commit_sync();
        void truncate();

        [[nodiscard]] bool needs_sync() const noexcept {
            return needs_group_sync_.load(std::memory_order_relaxed);
        }

    private:
        void sync_file();

        std::filesystem::path path_;
        std::ofstream file_;
        std::mutex mutex_;
        std::atomic<bool> needs_group_sync_{ false };
    };

    // ---- WAL 管理器 ----
    /** @brief 管理多张表 WAL 写入器的单例。 */
    class WalManager {
    public:
        static WalManager& instance();

        WalWriter& get_writer(const std::filesystem::path& path);
        void remove_writer(const std::filesystem::path& path);
        void truncate(const std::filesystem::path& path);
        void clear_all();
        void sync_all_pending();
        [[nodiscard]] bool sync_all_pending_any();

    private:
        WalManager() = default;

        std::mutex mutex_;
        std::unordered_map<std::string, std::unique_ptr<WalWriter>> writers_;
    };

    // ---- Group Commit ----
    /** @brief WAL Group Commit 协调器（Leader-Follower 模式）。 */
    class WalGroupCommitter {
    public:
        static WalGroupCommitter& instance();

        void request_sync(WalWriter& writer);
        void shutdown();

    private:
        WalGroupCommitter() = default;
        void do_sync();

        std::mutex mutex_;
        std::condition_variable cv_;
        uint64_t current_epoch_{ 0 };
        uint32_t pending_count_{ 0 };
        bool shutdown_{ false };
    };

    void wal_group_commit(WalWriter& writer);
    void wal_append_record(const std::filesystem::path& path, uint8_t type, const std::string& payload);
    void wal_append_record_no_wait(const std::filesystem::path& path, uint8_t type, const std::string& payload);
    void wal_sync_path(const std::filesystem::path& path);
    [[nodiscard]] std::vector<WalRecord> wal_read_records(const std::filesystem::path& path);

    // ---- Schema/Row 序列化 ----
    [[nodiscard]] std::vector<char> serialize_schema(const std::vector<Column>& columns);
    [[nodiscard]] std::vector<Column> deserialize_schema(std::istream& is, const std::string& table_name);

    void write_value(std::ostream& os, const Value& v);
    [[nodiscard]] Value read_value(std::istream& is);

    [[nodiscard]] std::vector<char> init_empty_page(std::size_t page_size = Config::kDefaultPageSize);
    bool append_record_to_page(std::vector<char>& page, const std::string& rec, uint64_t lsn);

    [[nodiscard]] std::string encode_row(const Row& row);
    [[nodiscard]] Row decode_row(const std::string& rec, const std::vector<Column>& columns,
                                 const std::string& table_name);

    [[nodiscard]] Row read_len_prefixed_row(std::istream& is, const std::vector<Column>& columns,
                                            const std::string& table_name);
    void write_len_prefixed_row(std::ostream& os, const Row& row);

    [[nodiscard]] int64_t extract_int_key(const Row& row);
    /** @brief 提取一行的主键 Value（约定首列；空行返回 NULL）。 */
    [[nodiscard]] Value extract_key(const Row& row);
    /** @brief 提取一行的主键 Value（schema 感知）：标记 PRIMARY KEY 的列 ≥2 时为复合主键
     *  （各 PK 列 encode_key 拼接的字符串 Value）；否则维持「首列即键」的历史约定。 */
    [[nodiscard]] Value extract_key(const Row& row, const std::vector<Column>& columns);
    /** @brief 将主键 Value 编码为字节串（复用 write_value，用于 WAL/SSTable/Bloom）。 */
    [[nodiscard]] std::string encode_key(const Value& key);
    /** @brief 从字节串解码主键 Value（与 encode_key 对应）。 */
    [[nodiscard]] Value decode_key(const std::string& bytes);
    [[nodiscard]] std::string index_file_path(const std::string& base_dir, const std::string& table,
                                              const std::string& column);
    void write_index_file(const std::string& path, const std::vector<std::pair<Value, Value>>& entries);
    [[nodiscard]] std::vector<std::pair<Value, Value>> read_index_file(const std::string& path);
    void compact_index_file(const std::string& path) ;

} // namespace corodb::storage_internal
