// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file lsm_storage_engine.cpp
// @brief 基于 LSM 结构的磁盘存储引擎的实现。

#include "corodb/storage/lsm_storage_engine.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "corodb/common/config.h"
#include "corodb/storage/storage_engine_common.h"

namespace corodb {
    using namespace storage_internal;

    /**
     * @brief LSM树存储引擎构造函数
     * @param base_dir 基础目录
     * @param buffer_pages 缓冲池页数
     */
    LSMTreeEngine::LSMTreeEngine(std::string base_dir, std::size_t buffer_pages)
        : base_dir_(std::move(base_dir)), memtable_limit_bytes_(Config::instance().memtable_size_bytes()),
          bufpool_(Config::instance().page_size(), buffer_pages) {
        ensure_base_dir();
        // Default GC horizon: no active snapshots outside a transaction manager,
        // so all old MVCC versions are safe to discard.  Database::Database()
        // overrides this with a real min_active_read_ts() callback.
        gc_horizon_fn_ = []() { return UINT64_MAX; };
    }

    /**
     * @brief 获取L0层SSTable文件的基础路径
     * @param name 表名
     * @return L0层SSTable文件路径
     */
    std::string LSMTreeEngine::base_path(const std::string& name) const {
        std::filesystem::path p(base_dir_);
        p /= name + ".lsm.L0"; // L0层SSTable文件格式：表名.lsm.L0
        return p.string();
    }

    /**
     * @brief 获取指定层SSTable文件的路径
     * @param name 表名
     * @param level 层级（从1开始）
     * @return 指定层SSTable文件路径
     */
    std::string LSMTreeEngine::level_path(const std::string& name, int level) const {
        std::filesystem::path p(base_dir_);
        p /= name + ".lsm.L" + std::to_string(level); // 层级文件格式：表名.lsm.LN，N为层级
        return p.string();
    }

    /**
     * @brief 获取WAL文件路径
     * @param name 表名
     * @return WAL文件路径
     */
    std::string LSMTreeEngine::wal_path(const std::string& name) const {
        std::filesystem::path p(base_dir_);
        p /= name + ".wal"; // WAL文件格式：表名.wal
        return p.string();
    }

    /**
     * @brief 确保基础目录存在，不存在则创建
     */
    void LSMTreeEngine::ensure_base_dir() const {
        std::error_code ec;
        std::filesystem::create_directories(base_dir_, ec);
        if (ec) {
            if (std::filesystem::exists(base_dir_) && std::filesystem::is_directory(base_dir_)) {
                return;
            }
            // 尝试使用绝对路径
            try {
                auto abs_path = std::filesystem::absolute(base_dir_);
                std::error_code ec2;
                std::filesystem::create_directories(abs_path, ec2);
                if (!ec2)
                    return;
            } catch (...) {
            }

            throw std::filesystem::filesystem_error("cannot create directories", base_dir_, ec);
        }
    }

    /**
     * @brief 检查表是否存在
     * @param name 表名
     * @return 如果表存在返回true，否则返回false
     */
    bool LSMTreeEngine::table_exists(const std::string& name) const {
        if (!std::filesystem::exists(base_dir_))
            return false;

        // 快速路径：检查WAL或L0文件是否存在
        if (std::filesystem::exists(wal_path(name)))
            return true;

        // 全面扫描：检查任何层级文件或索引
        const std::string prefix_lsm = name + ".lsm.";
        const std::string idx_prefix = name + ".";

        for (const auto& entry: std::filesystem::directory_iterator(base_dir_)) {
            if (!entry.is_regular_file())
                continue;
            std::string fname = entry.path().filename().string();

            // 检查LSM层级文件
            if (fname.find(prefix_lsm) == 0)
                return true;

            // 检查索引文件
            // 为避免误匹配（如表"users"匹配"users_backup.idx"），
            // 确保前缀"users."完全匹配
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".idx") {
                if (fname.find(idx_prefix) == 0)
                    return true;
            }
        }
        return false;
    }

    /**
     * @brief 删除表的所有文件
     * @param name 表名
     */
    void LSMTreeEngine::drop_table_files(const std::string& name) {
        if (!std::filesystem::exists(base_dir_))
            return;

        // 在删除文件前必须释放 WAL 写入器，否则在 Windows 上文件句柄
        // 仍被持有，filesystem::remove 会静默失败，造成"幽灵表"
        WalManager::instance().remove_writer(wal_path(name));

        // 遍历目录删除所有匹配的文件（L0, L1.., WAL, IDX）
        // 这比猜测层级更健壮，可以处理空隙和意外文件
        const std::string prefix_lsm = name + ".lsm.";
        const std::string wal_name = name + ".wal";
        const std::string prefix_idx = name + ".";

        for (const auto& entry: std::filesystem::directory_iterator(base_dir_)) {
            if (!entry.is_regular_file())
                continue;
            std::string fname = entry.path().filename().string();

            bool should_delete = false;

            // 检查WAL文件
            if (fname == wal_name)
                should_delete = true;

            // 检查LSM层级文件（name.lsm.L*）
            else if (fname.find(prefix_lsm) == 0)
                should_delete = true;

            // 检查索引文件（name.column.idx）
            // 文件名格式：表名 + "." + 列名 + ".idx"
            else if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".idx") {
                if (fname.find(prefix_idx) == 0) {
                    // 验证是否为该表的索引，而非名为"name_suffix"的表
                    // 由于使用"."作为分隔符，"name."前缀可以安全匹配"name"
                    should_delete = true;
                }
            }

            if (should_delete) {
                try {
                    std::filesystem::remove(entry.path());
                } catch (...) {
                    // 忽略删除错误（如文件被其他进程打开）
                }
            }
        }
    }

    /**
     * @brief 获取表状态（如果不存在则创建）
     */
    LSMTreeEngine::TableState* LSMTreeEngine::get_or_create_state(const std::string& name) const {
        {
            std::shared_lock lock(states_mutex_);
            auto it = states_.find(name);
            if (it != states_.end()) {
                return it->second.get();
            }
        }
        std::unique_lock lock(states_mutex_);
        auto it = states_.find(name);
        if (it != states_.end()) {
            return it->second.get();
        }
        auto state = std::make_unique<TableState>();
        auto* ptr = state.get();
        states_.emplace(name, std::move(state));
        return ptr;
    }

    /**
     * @brief 创建LSM树表
     * @param name 表名
     * @param columns 列定义列表
     */
    void LSMTreeEngine::create_table(const std::string& name, const std::vector<Column>& columns) {
        ensure_base_dir(); // 确保基础目录存在

        auto* state = get_or_create_state(name);
        std::unique_lock lock(state->mutex); // 独占锁保护表状态

        // 重置状态
        state->schema = columns;
        state->memtable.clear();
        state->memtable_bytes = 0;

        // 安全起见，删除可能存在的旧文件
        drop_table_files(name);

        write_sstable(base_path(name), columns, std::vector<MemEntry>{});

        // Create empty WAL and fsync it.
        {
            std::ofstream wal(wal_path(name), std::ios::binary | std::ios::trunc);
            uint32_t magic = kWalMagic;
            wal.write(reinterpret_cast<char*>(&magic), sizeof(magic));
            wal.close();
        }
        if (storage_internal::get_wal_sync_mode() == storage_internal::WalSyncMode::Durable)
            fsync_path(wal_path(name));
    }

    /**
     * @brief 加载表结构
     * @param name 表名
     * @return 列定义列表
     */
    std::vector<Column> LSMTreeEngine::load_schema(const std::string& name) const {
        auto* state = get_or_create_state(name);
        std::unique_lock lock(state->mutex); // 需要独占锁来加载状态
        if (!state->schema.empty()) {
            return state->schema; // 已加载，直接返回
        }
        load_state_locked(state, name, {}); // 直接调用内部方法，不会重复加锁
        return state->schema;               // 返回表结构
    }

    /**
     * @brief 读取SSTable文件的所有行数据
     * @param path SSTable文件路径
     * @param columns 列定义列表
     * @return 行数据列表
     */
    std::shared_ptr<const std::vector<LSMTreeEngine::MemEntry>>
    LSMTreeEngine::read_sstable(const std::string& path, const std::vector<Column>& columns) const {
        const auto abs_path = std::filesystem::absolute(path).string();
        std::error_code ec;
        if (!std::filesystem::exists(abs_path, ec) || ec) {
            return std::make_shared<const std::vector<MemEntry>>();
        }
        const auto mtime = std::filesystem::last_write_time(abs_path, ec).time_since_epoch().count();
        if (ec) {
            // mtime 不可用时退化为不缓存的解码
        } else {
            std::shared_lock l_shared(sst_decoded_cache_mutex_);
            auto it = sst_decoded_cache_.find(abs_path);
            if (it != sst_decoded_cache_.end() && it->second.first == mtime) {
                return it->second.second;
            }
        }
        // Note: LRU promotion on cache hit is skipped under shared_lock.
        // Compaction replaces SSTable files, so hits are bounded by file lifetime.

        auto rows_owned = std::make_shared<std::vector<MemEntry>>();
        std::vector<MemEntry>& rows = *rows_owned;
        std::ifstream ifs(path, std::ios::binary); // 打开SSTable文件
        if (!ifs)
            return rows_owned;
        uint32_t magic = 0;
        uint32_t schema_bytes = 0;
        ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        ifs.read(reinterpret_cast<char*>(&schema_bytes), sizeof(schema_bytes));
        if (!ifs || magic != kLsmMagic)
            return rows_owned;
        std::vector<char> schema_buf(schema_bytes);
        ifs.read(schema_buf.data(), static_cast<std::streamsize>(schema_buf.size()));

        std::size_t pos = sizeof(magic) + sizeof(schema_bytes) + schema_buf.size();
        std::size_t aligned = align_up(pos, Config::kDefaultPageSize);

        std::size_t file_size = std::filesystem::file_size(abs_path);
        if (file_size < aligned)
            return rows_owned;
        std::size_t data_bytes = file_size - aligned;
        uint32_t offset_pages = static_cast<uint32_t>(aligned / Config::kDefaultPageSize);
        uint32_t page_count =
                data_bytes == 0
                        ? 0
                        : static_cast<uint32_t>((data_bytes + Config::kDefaultPageSize - 1) / Config::kDefaultPageSize);

        std::string buffer;
        buffer.reserve(data_bytes);
        for (uint32_t i = 0; i < page_count; ++i) {
            PageId pid{ abs_path, offset_pages + i };
            auto frame = bufpool_.pin(pid);
            // Verify page checksum even when read from cache (defense-in-depth).
            auto* hdr = reinterpret_cast<PageHeader*>(frame->data.data());
            if (hdr->checksum != 0) {
                uint32_t computed = compute_page_checksum(frame->data);
                if (computed != hdr->checksum) {
                    bufpool_.unpin(frame);
                    throw std::runtime_error("SSTable page checksum mismatch: " + path +
                                             " page=" + std::to_string(offset_pages + i));
                }
            }
            std::size_t copied = std::min<std::size_t>(
                    Config::kDefaultPageSize, data_bytes - static_cast<std::size_t>(i) * Config::kDefaultPageSize);
            buffer.append(frame->data.data(), copied);
            bufpool_.unpin(frame);
        }

        rows.reserve(data_bytes / 100 + 1);

        std::istringstream iss(buffer);
        while (iss.peek() != std::char_traits<char>::eof()) {
            uint8_t type = 0;
            iss.read(reinterpret_cast<char*>(&type), sizeof(type));
            uint64_t commit_ts = 0;
            iss.read(reinterpret_cast<char*>(&commit_ts), sizeof(commit_ts));
            uint32_t len = 0;
            iss.read(reinterpret_cast<char*>(&len), sizeof(len));
            if (!iss)
                break;
            std::string rec(len, '\0');
            iss.read(rec.data(), len);
            if (!iss)
                break;
            if (type == 1) {
                Row row = decode_row(rec, columns, path);
                int64_t pk = row.values.empty() ? 0 : extract_int_key(row);
                rows.emplace_back(MemEntry{ pk, std::move(row), false, commit_ts });
            } else if (type == 2) {
                // V2 tombstone now stores [8B pk] in record bytes; legacy may have len=0.
                int64_t pk = 0;
                if (rec.size() >= sizeof(int64_t)) {
                    std::memcpy(&pk, rec.data(), sizeof(int64_t));
                }
                rows.emplace_back(MemEntry{ pk, Row{}, true, commit_ts });
            }
        }

        std::shared_ptr<const std::vector<MemEntry>> result = rows_owned;
        if (!ec) {
            std::unique_lock l(sst_decoded_cache_mutex_);
            const std::size_t max_entries = Config::instance().sst_cache_entries();
            while (sst_decoded_cache_.size() >= max_entries && !sst_cache_lru_.empty()) {
                const std::string& lru_key = sst_cache_lru_.front();
                sst_decoded_cache_.erase(lru_key);
                sst_cache_lru_.pop_front();
            }
            sst_decoded_cache_[abs_path] = { mtime, result };
            sst_cache_lru_.push_back(abs_path);
        }

        // Load footer for bloom filter (best-effort: ignore failures).
        {
            std::shared_lock lf(sst_footer_cache_mutex_);
            auto fit = sst_footer_cache_.find(abs_path);
            if (fit != sst_footer_cache_.end() && fit->second.first == mtime) {
                // Footer already cached.
            } else {
                lf.unlock();
                SstFooter footer;
                std::ifstream ifs_footer(path, std::ios::binary);
                if (ifs_footer) {
                    ifs_footer.seekg(0, std::ios::end);
                    std::streamoff fsize = ifs_footer.tellg();
                    std::size_t file_len = static_cast<std::size_t>(fsize);
                    if (file_len > sizeof(uint32_t)) {
                        // Read last ~256 bytes (should contain the footer).
                        std::size_t seek_offset = (file_len > 256U) ? (file_len - 256U) : 0U;
                        ifs_footer.seekg(static_cast<std::streamoff>(seek_offset));
                        std::size_t tail_len = file_len - seek_offset;
                        std::string tail(tail_len, '\0');
                        ifs_footer.read(tail.data(), static_cast<std::streamsize>(tail.size()));
                        uint32_t fmagic = kFooterMagic;
                        auto magic_bytes = std::string(reinterpret_cast<const char*>(&fmagic), sizeof(fmagic));
                        auto footer_pos = tail.rfind(magic_bytes);
                        if (footer_pos != std::string::npos) {
                            footer.deserialize(tail.substr(footer_pos));
                        }
                    }
                }
                if (!ec) {
                    std::unique_lock lf2(sst_footer_cache_mutex_);
                    sst_footer_cache_[abs_path] = std::make_pair(mtime, std::move(footer));
                }
            }
        }

        return result;
    }

    /**
     * @brief 写入SSTable文件
     * @param path SSTable文件路径
     * @param columns 列定义列表
     * @param rows 行数据列表
     */
    void LSMTreeEngine::write_sstable(const std::string& path, const std::vector<Column>& columns,
                                      const std::vector<MemEntry>& entries) const {
        // Write to a temporary file first, then atomically rename to the final
        // path.  This prevents corruption from a crash mid-write and avoids the
        // window where a truncated file replaces valid data before the new data
        // is fully durable.
        const auto abs_final = std::filesystem::absolute(path).string();
        const std::string tmp_path = path + ".tmp";

        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("Failed to write SSTable: " + path);
        auto schema_buf = serialize_schema(columns);
        uint32_t magic = kLsmMagic;
        uint32_t schema_bytes = static_cast<uint32_t>(schema_buf.size());
        ofs.write(reinterpret_cast<char*>(&magic), sizeof(magic));
        ofs.write(reinterpret_cast<char*>(&schema_bytes), sizeof(schema_bytes));
        ofs.write(schema_buf.data(), static_cast<std::streamsize>(schema_buf.size()));

        std::size_t pos = sizeof(magic) + sizeof(schema_bytes) + schema_buf.size();
        std::size_t aligned = align_up(pos, Config::kDefaultPageSize);
        std::vector<char> padding(aligned - pos, 0);
        ofs.write(padding.data(), static_cast<std::streamsize>(padding.size()));
        ofs.close();

        std::string payload;
        for (const auto& e: entries) {
            uint8_t type = e.tombstone ? 2 : 1;
            std::string rec;
            if (e.tombstone) {
                rec.assign(sizeof(int64_t), '\0');
                int64_t pk = e.pk;
                std::memcpy(rec.data(), &pk, sizeof(int64_t));
            } else {
                rec = encode_row(e.row);
            }
            uint64_t ts = e.commit_ts;
            uint32_t len = static_cast<uint32_t>(rec.size());
            payload.append(reinterpret_cast<char*>(&type), sizeof(type));
            payload.append(reinterpret_cast<char*>(&ts), sizeof(ts));
            payload.append(reinterpret_cast<char*>(&len), sizeof(len));
            payload.append(rec.data(), rec.size());
        }

        std::size_t data_bytes = payload.size();
        uint32_t offset_pages = static_cast<uint32_t>(aligned / Config::kDefaultPageSize);
        uint32_t page_count =
                data_bytes == 0
                        ? 0
                        : static_cast<uint32_t>((data_bytes + Config::kDefaultPageSize - 1) / Config::kDefaultPageSize);
        const auto abs_tmp = std::filesystem::absolute(tmp_path).string();
        for (uint32_t i = 0; i < page_count; ++i) {
            std::vector<char> page(Config::kDefaultPageSize, 0);
            std::size_t start = static_cast<std::size_t>(i) * Config::kDefaultPageSize;
            std::size_t copy = std::min<std::size_t>(Config::kDefaultPageSize, data_bytes - start);
            std::memcpy(page.data(), payload.data() + start, copy);
            PageId pid{ abs_tmp, offset_pages + i };
            auto frame = bufpool_.allocate(pid);
            frame->data = std::move(page);
            bufpool_.mark_dirty(frame);
            bufpool_.unpin(frame);
        }
        bufpool_.flush_all();

        // Write footer with bloom filter for point-query optimization.
        {
            SstFooter footer;
            if (!entries.empty()) {
                footer.min_pk = entries.front().pk;
                footer.max_pk = entries.back().pk;
                // Collect unique pks for bloom filter.
                std::vector<int64_t> pks;
                pks.reserve(entries.size());
                int64_t prev = 0;
                bool first = true;
                for (const auto& e: entries) {
                    if (first || e.pk != prev) {
                        pks.push_back(e.pk);
                        prev = e.pk;
                        first = false;
                    }
                }
                footer.bloom.build(pks);
            }
            std::string footer_data = footer.serialize();
            std::ofstream ofs_footer(tmp_path, std::ios::binary | std::ios::app);
            ofs_footer.write(footer_data.data(), static_cast<std::streamsize>(footer_data.size()));
        }

        // Fsync the temp file before renaming so the rename is guaranteed to
        // replace the old file with fully durable data.
        fsync_path(tmp_path);

        // Atomic rename: replace the old SSTable with the new one.
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            // Fallback: try remove-then-rename on platforms where rename over
            // existing file isn't atomic (rare).
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmp_path, path, ec);
            if (ec)
                throw std::runtime_error("Failed to rename SSTable: " + path);
        }
    }

    /**
     * @brief 加载表状态（如果需要）
     * @param name 表名
     * @param columns 列定义列表
     */
    void LSMTreeEngine::load_state_locked(TableState* state, const std::string& name,
                                          const std::vector<Column>& columns) const {
        (void)columns; // 未使用
        // 注意：调用方必须持有 state->mutex (Exclusive)
        if (!state->schema.empty())
            return; // 状态已加载

        std::ifstream ifs(base_path(name), std::ios::binary); // 打开L0层SSTable
        if (!ifs)
            throw std::runtime_error("LSM table not found: " + name);           // 表不存在
        uint32_t magic = 0, schema_bytes = 0;                                   // 魔数和表结构字节数
        ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));               // 读取魔数
        ifs.read(reinterpret_cast<char*>(&schema_bytes), sizeof(schema_bytes)); // 读取表结构字节数
        if (!ifs || magic != kLsmMagic)
            throw std::runtime_error("Invalid LSM table header: " + name);
        std::vector<char> schema_buf(schema_bytes);                                   // 分配空间存储表结构
        ifs.read(schema_buf.data(), static_cast<std::streamsize>(schema_buf.size())); // 读取表结构
        std::istringstream iss(std::string(schema_buf.data(), schema_buf.size()));    // 创建字符串流
        auto schema = deserialize_schema(iss, name);                                  // 反序列化表结构
        state->schema = schema;                                                       // 保存表结构

        // 读取WAL日志，恢复内存表（多版本）
        // WAL 记录类型：
        //   1  = insert（旧格式，无 commit_ts）
        //   2  = tombstone（旧格式）
        //   11 = insert with commit_ts（payload = [8B ts][row_bytes]）
        //   12 = tombstone with commit_ts（payload = [8B ts][8B key]）
        for (const auto& rec: wal_read_records(wal_path(name))) {
            if (rec.type == 2) {
                if (rec.payload.size() < sizeof(int64_t))
                    continue;
                int64_t key = 0;
                std::memcpy(&key, rec.payload.data(), sizeof(int64_t));
                state->memtable[MVCCKey{ key, 0 }] = MemEntry{ key, Row{}, true, 0 };
            } else if (rec.type == 1) {
                Row r = decode_row(rec.payload, schema, name);
                int64_t key = extract_int_key(r);
                state->memtable[MVCCKey{ key, 0 }] = MemEntry{ key, r, false, 0 };
            } else if (rec.type == 12) {
                if (rec.payload.size() < sizeof(uint64_t) + sizeof(int64_t))
                    continue;
                uint64_t ts = 0;
                int64_t key = 0;
                std::memcpy(&ts, rec.payload.data(), sizeof(uint64_t));
                std::memcpy(&key, rec.payload.data() + sizeof(uint64_t), sizeof(int64_t));
                state->memtable[MVCCKey{ key, ts }] = MemEntry{ key, Row{}, true, ts };
            } else if (rec.type == 11) {
                if (rec.payload.size() < sizeof(uint64_t))
                    continue;
                uint64_t ts = 0;
                std::memcpy(&ts, rec.payload.data(), sizeof(uint64_t));
                std::string row_bytes = rec.payload.substr(sizeof(uint64_t));
                Row r = decode_row(row_bytes, schema, name);
                int64_t key = extract_int_key(r);
                state->memtable[MVCCKey{ key, ts }] = MemEntry{ key, r, false, ts };
            }
        }
    }

    /**
     * @brief 按需加载表状态
     * @param name 表名
     * @param columns 列定义列表
     */
    void LSMTreeEngine::load_state_if_needed(const std::string& name, const std::vector<Column>& columns) const {
        auto* state = get_or_create_state(name);
        {
            std::shared_lock lock(state->mutex);
            if (!state->schema.empty())
                return;
        }
        std::unique_lock lock(state->mutex);
        load_state_locked(state, name, columns);
    }

    void LSMTreeEngine::flush_memtable_internal(const std::string& name, const std::vector<Column>& columns,
                                                std::map<MVCCKey, MemEntry, MVCCKeyCompare> mem_snapshot) {
        // 注意：此函数在不持有 state->mutex 的情况下执行
        // mem_snapshot 是从 state->memtable 移动出来的快照（多版本，sorted by MVCCKey）

        auto l0_ptr = read_sstable(base_path(name), columns); // 读取L0层SSTable（已按 MVCCKey 排序）
        std::vector<MemEntry> l0_entries(l0_ptr->begin(), l0_ptr->end());

        // 双流合并：l0_entries 与 mem_snapshot 都按 (pk asc, ts desc) 排序，按 MVCCKey 全序合并并去重。
        std::vector<MemEntry> merged;
        merged.reserve(l0_entries.size() + mem_snapshot.size());

        MVCCKeyCompare cmp;
        auto it_disk = l0_entries.begin();
        auto it_mem = mem_snapshot.begin();
        auto disk_key = [](const MemEntry& e) { return MVCCKey{ e.pk, e.commit_ts }; };

        while (it_disk != l0_entries.end() || it_mem != mem_snapshot.end()) {
            if (it_mem == mem_snapshot.end()) {
                merged.push_back(std::move(*it_disk++));
            } else if (it_disk == l0_entries.end()) {
                merged.push_back(std::move(it_mem->second));
                ++it_mem;
            } else {
                MVCCKey dk = disk_key(*it_disk);
                MVCCKey mk = it_mem->first;
                if (cmp(mk, dk)) {
                    merged.push_back(std::move(it_mem->second));
                    ++it_mem;
                } else if (cmp(dk, mk)) {
                    merged.push_back(std::move(*it_disk++));
                } else {
                    // 同 (pk, ts) → memtable 优先（更近写入）
                    merged.push_back(std::move(it_mem->second));
                    ++it_mem;
                    ++it_disk;
                }
            }
        }

        // MVCC: 保留所有版本（含 tombstone），由后续 compaction GC

        write_sstable(base_path(name), columns, merged);

        if (pool_) {
            pool_->submit([this, name, columns]() { compact_levels(name, columns); });
        }

        // Truncate WAL via WalWriter so the header write is fsync'd.
        WalManager::instance().truncate(wal_path(name));
    }

    /**
     * @brief 刷新内存表到L0层SSTable
     * @param name 表名
     * @param columns 列定义列表
     */
    void LSMTreeEngine::flush_memtable(const std::string& name, const std::vector<Column>& columns) {
        auto* state = get_or_create_state(name);
        // 将 memtable 移出到本地快照，然后释放表级锁以在不持锁的情况下执行磁盘 I/O
        std::map<MVCCKey, MemEntry, MVCCKeyCompare> mem_snapshot;
        {
            std::unique_lock lock(state->mutex); // 独占锁
            mem_snapshot = std::move(state->memtable);
            state->memtable.clear();
            state->memtable_bytes = 0;
        }
        // 在没有持有 state->mutex 的情况下执行实际的刷写工作
        flush_memtable_internal(name, columns, std::move(mem_snapshot));
    }

    /**
     * @brief 加载LSM树表的所有行数据（latest committed view）
     */
    std::vector<Row> LSMTreeEngine::load_rows(const std::string& name, const std::vector<Column>& columns) {
        return scan_visible(name, columns, std::numeric_limits<uint64_t>::max());
    }

    /**
     * @brief 向LSM树表中追加一行数据
     * @param name 表名
     * @param columns 列定义列表
     * @param row 行数据
     */
    void LSMTreeEngine::append_row(const std::string& name, const std::vector<Column>& columns, const Row& row,
                                   uint64_t commit_ts) {
        auto* state = get_or_create_state(name);
        std::unique_lock lock(state->mutex);     // 独占锁保护表状态
        load_state_locked(state, name, columns); // 加载状态（如果需要）

        int64_t key = extract_int_key(row); // 提取键值

        std::string rec = encode_row(row);       // 编码行数据（复用于WAL和大小计算）
        const std::size_t rec_size = rec.size(); // 保存编码后的大小

        // MVCC 多版本：直接 emplace (pk, commit_ts) 新版本，不覆盖既有版本
        state->memtable[MVCCKey{ key, commit_ts }] = MemEntry{ key, row, false, commit_ts };
        state->memtable_bytes += rec_size;
        state->write_version.fetch_add(1, std::memory_order_release);

        // WAL：commit_ts > 0 走 V2 类型 11（payload 前置 8B ts），否则维持类型 1。
        const auto wpath = wal_path(name);
        if (commit_ts != 0) {
            std::string ext_payload(sizeof(uint64_t), '\0');
            std::memcpy(ext_payload.data(), &commit_ts, sizeof(uint64_t));
            ext_payload.append(rec);
            wal_append_record_no_wait(wpath, 11, ext_payload);
        } else {
            wal_append_record_no_wait(wpath, 1, rec);
        }

        // 检查内存表大小是否超过限制
        bool need_flush = state->memtable_bytes >= memtable_limit_bytes_;
        std::map<MVCCKey, MemEntry, MVCCKeyCompare> mem_snapshot;
        if (need_flush) {
            mem_snapshot = std::move(state->memtable);
            state->memtable.clear();
            state->memtable_bytes = 0;
        }
        lock.unlock();

        wal_sync_path(wpath);

        if (need_flush) {
            flush_memtable_internal(name, columns, std::move(mem_snapshot));
        }
    }

    void LSMTreeEngine::delete_row_by_key(const std::string& name, const std::vector<Column>& columns, int64_t key,
                                          uint64_t commit_ts) {
        auto* state = get_or_create_state(name);
        std::unique_lock lock(state->mutex);
        load_state_locked(state, name, columns);

        // MVCC 多版本：emplace tombstone 新版本，不覆盖既有版本
        state->memtable[MVCCKey{ key, commit_ts }] = MemEntry{ key, Row{}, true, commit_ts };
        state->write_version.fetch_add(1, std::memory_order_release);
        // tombstone 不参与 memtable_bytes 增量（接近零），保持简单

        const auto wpath = wal_path(name);
        if (commit_ts != 0) {
            // V2 墓碑：payload = [8B ts][8B key]
            std::string payload(sizeof(uint64_t) + sizeof(int64_t), '\0');
            std::memcpy(payload.data(), &commit_ts, sizeof(uint64_t));
            std::memcpy(payload.data() + sizeof(uint64_t), &key, sizeof(int64_t));
            wal_append_record_no_wait(wpath, 12, payload);
        } else {
            std::string payload(sizeof(int64_t), '\0');
            std::memcpy(payload.data(), &key, sizeof(int64_t));
            wal_append_record_no_wait(wpath, 2, payload);
        }

        lock.unlock();
        wal_sync_path(wpath);
    }

    uint64_t LSMTreeEngine::max_observed_commit_ts() const {
        uint64_t max_ts = 0;
        std::error_code ec;
        if (!std::filesystem::exists(base_dir_, ec) || ec)
            return 0;

        // 扫描 WAL 文件：解析记录，type=11/12 时取 payload 前 8 字节
        for (const auto& entry: std::filesystem::directory_iterator(base_dir_, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            const auto& p = entry.path();
            const auto ext = p.extension().string();

            if (ext == ".wal") {
                try {
                    for (const auto& rec: wal_read_records(p)) {
                        if ((rec.type == 11 || rec.type == 12) && rec.payload.size() >= sizeof(uint64_t)) {
                            uint64_t ts = 0;
                            std::memcpy(&ts, rec.payload.data(), sizeof(uint64_t));
                            if (ts > max_ts)
                                max_ts = ts;
                        }
                    }
                } catch (...) { /* skip corrupt WAL */
                }
                continue;
            }

            // SSTable: 文件名形如 <table>.lsm.LN
            const std::string fname = p.filename().string();
            if (fname.find(".lsm.L") == std::string::npos)
                continue;

            try {
                std::ifstream ifs(p, std::ios::binary);
                if (!ifs)
                    continue;
                uint32_t magic = 0, schema_bytes = 0;
                ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
                ifs.read(reinterpret_cast<char*>(&schema_bytes), sizeof(schema_bytes));
                if (!ifs)
                    continue;
                if (magic != kLsmMagic)
                    continue;                           // unknown magic
                ifs.seekg(schema_bytes, std::ios::cur); // skip schema header
                while (ifs && ifs.peek() != EOF) {
                    uint8_t type = 0;
                    uint64_t ts = 0;
                    uint32_t len = 0;
                    ifs.read(reinterpret_cast<char*>(&type), sizeof(type));
                    ifs.read(reinterpret_cast<char*>(&ts), sizeof(ts));
                    ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
                    if (!ifs)
                        break;
                    if (ts > max_ts)
                        max_ts = ts;
                    ifs.seekg(len, std::ios::cur);
                }
            } catch (...) { /* skip corrupt SSTable */
            }
        }
        return max_ts;
    }

    std::optional<Row> LSMTreeEngine::lookup_visible(const std::string& name, const std::vector<Column>& columns,
                                                     int64_t pk, uint64_t snapshot_ts) {
        load_state_if_needed(name, columns);
        auto* state = get_or_create_state(name);
        std::shared_lock lock(state->mutex);

        // 收集来自 memtable + 所有 SSTable 的、属于该 pk 的所有版本
        struct Cand {
            uint64_t ts;
            bool tombstone;
            Row row;
        };
        std::vector<Cand> cands;

        // memtable: lower_bound({pk, snapshot_ts}) 直接落在第一个 ts <= snapshot_ts 的版本
        {
            auto it = state->memtable.lower_bound(MVCCKey{ pk, snapshot_ts });
            // 若 snapshot_ts == UINT64_MAX，需要从 pk 的第一个版本开始（ts desc）
            // lower_bound 找第一个不小于 (pk, snapshot_ts) 的 key；当 snapshot_ts == max，所有该 pk 的 key 都小于它，
            // 故 lower_bound 跳过该 pk 的所有版本。修正：先把 it 退到该 pk 的最高版本。
            // 最简方法：从 lower_bound({pk, UINT64_MAX}) 等效，遍历该 pk 全部条目取首个 ts <= snapshot_ts 即可。
            // 这里改为从 pk 起始扫描。
            (void)it;
            for (auto jt = state->memtable.lower_bound(MVCCKey{ pk, std::numeric_limits<uint64_t>::max() });
                 jt != state->memtable.end() && jt->first.pk == pk; ++jt) {
                if (jt->first.commit_ts <= snapshot_ts) {
                    cands.push_back({ jt->second.commit_ts, jt->second.tombstone, jt->second.row });
                    break; // 排序保证首个 <= snapshot_ts 即为可见版本
                }
            }
        }

        // SSTable: 扫描全部层级（V2），过滤同 pk
        int max_level = 0;
        while (std::filesystem::exists(level_path(name, max_level + 1)))
            ++max_level;
        for (int l = 0; l <= max_level; ++l) {
            std::string p = (l == 0) ? base_path(name) : level_path(name, l);
            if (!std::filesystem::exists(p))
                continue;

            // Bloom filter check: skip reading SSTable if key is definitely absent.
            {
                auto abs_p = std::filesystem::absolute(p).string();
                std::shared_lock lf(sst_footer_cache_mutex_);
                auto fit = sst_footer_cache_.find(abs_p);
                if (fit != sst_footer_cache_.end() && fit->second.second.valid()) {
                    if (pk < fit->second.second.min_pk || pk > fit->second.second.max_pk ||
                        !fit->second.second.bloom.maybe_contains(pk)) {
                        continue; // Key definitely not in this SSTable.
                    }
                }
            }

            auto ptr = read_sstable(p, columns);
            // SST 按 (pk asc, ts desc) 排序：用二分定位 pk 起点，避免 O(N) 线性扫描。
            auto lo = std::lower_bound(ptr->begin(), ptr->end(), pk,
                                       [](const MemEntry& e, int64_t key) { return e.pk < key; });
            for (auto it = lo; it != ptr->end() && it->pk == pk; ++it) {
                if (it->commit_ts <= snapshot_ts) {
                    cands.push_back({ it->commit_ts, it->tombstone, it->row });
                    break; // 该 pk 内 ts desc，首个 <= snapshot_ts 即可见版本
                }
            }
        }

        if (cands.empty())
            return std::nullopt;

        // 取 commit_ts 最大的可见版本
        auto best =
                std::max_element(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.ts < b.ts; });
        if (best->tombstone)
            return std::nullopt;
        return best->row;
    }

    std::vector<Row> LSMTreeEngine::scan_visible(const std::string& name, const std::vector<Column>& columns,
                                                 uint64_t snapshot_ts) {
        load_state_if_needed(name, columns);
        auto* state = get_or_create_state(name);

        // Fast path: cache hit — shared locks only, fully concurrent.
        // Cache 复用条件：
        //   1) write_version 未变（无并发写入）
        //   2) snapshot_ts >= scan_cache_ts （新查询不会少看到东西）
        //   3) scan_cache_ts >= scan_cache_max_commit_ts （上次重建时已覆盖所有现有版本）
        // 第 3 条保证：在 (cache_ts, snapshot_ts] 区间内不存在被遗漏的历史版本。
        {
            std::shared_lock sl(state->mutex);
            uint64_t cur_v = state->write_version.load(std::memory_order_acquire);
            std::shared_lock cl(state->scan_cache_mutex);
            if (state->scan_cache_valid && state->scan_cache_version == cur_v && snapshot_ts >= state->scan_cache_ts &&
                state->scan_cache_ts >= state->scan_cache_max_commit_ts) {
                return state->scan_cache;
            }
        }

        // Cache miss: serialize rebuilds to prevent thundering herd.
        // Only one thread rebuilds; others double-check after the rebuilder finishes.
        std::unique_lock rebuild_guard(state->cache_rebuild_mutex);

        // Re-acquire state->mutex shared_lock to stabilize memtable during scan.
        std::shared_lock lock(state->mutex);
        uint64_t cur_v = state->write_version.load(std::memory_order_acquire);

        // Double-check: another thread may have rebuilt the cache while we waited.
        {
            std::shared_lock cl(state->scan_cache_mutex);
            if (state->scan_cache_valid && state->scan_cache_version == cur_v && snapshot_ts >= state->scan_cache_ts &&
                state->scan_cache_ts >= state->scan_cache_max_commit_ts) {
                return state->scan_cache;
            }
        }

        // 收集每个 pk 的可见最大 ts 版本：(pk -> (ts, tombstone, row))
        struct Best {
            uint64_t ts{ 0 };
            bool tombstone{ false };
            bool seen{ false };
            Row row;
        };
        std::map<int64_t, Best> best;
        uint64_t observed_max_commit_ts = 0;

        auto consider = [&](int64_t pk, uint64_t ts, bool tomb, Row&& row) {
            if (ts > observed_max_commit_ts)
                observed_max_commit_ts = ts;
            if (ts > snapshot_ts)
                return;
            auto& b = best[pk];
            if (!b.seen || ts > b.ts) {
                b.ts = ts;
                b.tombstone = tomb;
                b.row = std::move(row);
                b.seen = true;
            }
        };

        // 1. SSTable 各层
        int max_level = 0;
        while (std::filesystem::exists(level_path(name, max_level + 1)))
            ++max_level;
        for (int l = 0; l <= max_level; ++l) {
            std::string p = (l == 0) ? base_path(name) : level_path(name, l);
            if (!std::filesystem::exists(p))
                continue;
            for (const auto& e: *read_sstable(p, columns)) {
                consider(e.pk, e.commit_ts, e.tombstone, Row{ e.row });
            }
        }
        // 2. memtable
        for (const auto& [k, e]: state->memtable) {
            Row r = e.row;
            consider(k.pk, k.commit_ts, e.tombstone, std::move(r));
        }

        std::vector<Row> result;
        result.reserve(best.size());
        for (auto& [pk, b]: best) {
            if (b.seen && !b.tombstone && !b.row.values.empty()) {
                result.push_back(std::move(b.row));
            }
        }

        // Update cache (still holding state->mutex shared_lock, so write_version is stable).
        {
            std::unique_lock cl(state->scan_cache_mutex);
            state->scan_cache = result;
            state->scan_cache_version = cur_v;
            state->scan_cache_ts = snapshot_ts;
            state->scan_cache_max_commit_ts = observed_max_commit_ts;
            state->scan_cache_valid = true;
        }

        return result;
    }

    /**
     * @brief 重写LSM树表的所有数据
     * @param name 表名
     * @param columns 列定义列表
     * @param rows 行数据列表
     */
    void LSMTreeEngine::rewrite_table(const std::string& name, const std::vector<Column>& columns,
                                      const std::vector<Row>& rows) {
        auto* state = get_or_create_state(name);
        std::unique_lock lock(state->mutex);     // 独占锁保护表状态
        load_state_locked(state, name, columns); // 加载状态（如果需要）
        state->memtable.clear();                 // 清空内存表
        state->memtable_bytes = 0;               // 重置内存表大小
        state->write_version.fetch_add(1, std::memory_order_release);

        // 先删除所有层级文件以避免过期数据
        drop_table_files(name);

        std::vector<MemEntry> entries;
        entries.reserve(rows.size());
        for (const auto& r: rows) {
            int64_t pk = r.values.empty() ? 0 : extract_int_key(r);
            entries.push_back(MemEntry{ pk, r, false, 0 });
        }
        write_sstable(base_path(name), columns, entries);

        // Truncate WAL so it only contains the header; fsync via WalWriter.
        WalManager::instance().truncate(wal_path(name));
    }

    /**
     * @brief 列出表的所有索引
     * @param table 表名
     * @return 索引列名列表
     */
    std::vector<std::string> LSMTreeEngine::list_indexes(const std::string& table) const {
        std::vector<std::string> cols; // 索引列名列表
        if (!std::filesystem::exists(base_dir_))
            return cols;                        // 基础目录不存在，返回空列表
        const std::string prefix = table + "."; // 索引文件前缀

        // 遍历基础目录中的所有文件
        for (const auto& entry: std::filesystem::directory_iterator(base_dir_)) {
            if (!entry.is_regular_file())
                continue;                                  // 跳过非普通文件
            auto fname = entry.path().filename().string(); // 获取文件名

            // 检查文件名是否符合索引文件格式：table.column.idx
            if (fname.size() <= prefix.size() + 4)
                continue; // 需要至少前缀+.idx
            if (fname.rfind(".idx") != fname.size() - 4)
                continue; // 后缀必须是.idx
            if (fname.compare(0, prefix.size(), prefix) != 0)
                continue; // 前缀必须匹配

            auto col = fname.substr(prefix.size(), fname.size() - prefix.size() - 4); // 提取列名
            cols.push_back(std::move(col));                                           // 添加到索引列名列表
        }
        return cols;
    }

    /**
     * @brief 删除表及其所有关联的文件
     * @param name 表名
     * @return true 如果表被成功删除，false 如果表不存在
     */
    bool LSMTreeEngine::drop_table(const std::string& name) {
        if (!table_exists(name)) {
            return false;
        }

        // 必须先释放 WAL 写入器持有的文件句柄，
        // 否则在 Windows 上 filesystem::remove 会静默失败
        WalManager::instance().remove_writer(wal_path(name));

        std::error_code ec;
        // 删除WAL文件
        std::filesystem::remove(base_dir_ + "/" + name + ".wal", ec);

        // 删除所有LSM层级文件 (L0, L1, L2, ...)
        for (int level = 0; level < 10; ++level) {
            auto path = base_dir_ + "/" + name + ".lsm.L" + std::to_string(level);
            if (!std::filesystem::exists(path))
                break;
            std::filesystem::remove(path, ec);
        }

        // 删除所有索引文件
        auto indexes = list_indexes(name);
        for (const auto& col: indexes) {
            auto idx_path = index_file_path(base_dir_, name, col);
            std::filesystem::remove(idx_path, ec);
        }

        // 清理内存中的表状态，避免后续 CREATE 看到陈旧 schema/memtable
        {
            std::unique_lock lock(states_mutex_);
            states_.erase(name);
        }

        return true;
    }

    /**
     * @brief 删除表的索引文件
     * @param table 表名
     * @param column 索引列名
     * @return true 如果索引被成功删除，false 如果索引不存在
     */
    bool LSMTreeEngine::drop_index(const std::string& table, const std::string& column) {
        auto path = index_file_path(base_dir_, table, column);
        if (!std::filesystem::exists(path)) {
            return false;
        }

        std::error_code ec;
        return std::filesystem::remove(path, ec);
    }

    /**
     * @brief 为指定表的指定列创建索引文件
     * @param table 表名
     * @param column 列名
     */
    void LSMTreeEngine::create_index_file(const std::string& table, const std::string& column) {
        auto path = index_file_path(base_dir_, table, column); // 获取索引文件路径
        if (std::filesystem::exists(path))
            return;                 // 如果文件已存在，直接返回
        write_index_file(path, {}); // 写入空索引文件
    }

    /**
     * @brief 写入索引行数据到索引文件
     * @param table 表名
     * @param column 列名
     * @param entries 索引条目列表
     */
    void LSMTreeEngine::write_index_rows(const std::string& table, const std::string& column,
                                         const std::vector<std::pair<Value, std::size_t>>& entries) {
        auto path = index_file_path(base_dir_, table, column); // 获取索引文件路径
        write_index_file(path, entries);                       // 写入索引条目
    }

    /**
     * @brief 从索引文件加载索引行数据
     * @param table 表名
     * @param column 列名
     * @return 索引条目列表
     */
    std::vector<std::pair<Value, std::size_t>> LSMTreeEngine::load_index_rows(const std::string& table,
                                                                              const std::string& column) const {
        auto path = index_file_path(base_dir_, table, column); // 获取索引文件路径
        return read_index_file(path);                          // 读取并返回索引条目
    }

    /**
     * @brief 将索引名注册表持久化为 `{table}.idx_names` 文件。
     * 格式：每行 `index_name:column_name`（UTF-8，LF 换行）。
     */
    void LSMTreeEngine::save_index_registry(const std::string& table,
                                            const std::unordered_map<std::string, std::string>& registry) {
        auto path = base_dir_ + "/" + table + ".idx_names";
        std::ofstream ofs(path, std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("[LSMTreeEngine] Cannot write index registry: " + path);
        for (const auto& [idx_name, col]: registry) {
            ofs << idx_name << ":" << col << "\n";
        }
    }

    /**
     * @brief 从 `{table}.idx_names` 文件加载索引名注册表。
     * 文件不存在时返回空 map（向后兼容）。
     */
    std::unordered_map<std::string, std::string> LSMTreeEngine::load_index_registry(const std::string& table) const {
        auto path = base_dir_ + "/" + table + ".idx_names";
        std::unordered_map<std::string, std::string> reg;
        std::ifstream ifs(path);
        if (!ifs)
            return reg; // 文件不存在属于正常情况
        std::string line;
        while (std::getline(ifs, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            reg[line.substr(0, colon)] = line.substr(colon + 1);
        }
        return reg;
    }

    /**
     * @brief 执行LSM树的分层压缩（Compaction）
     * @param name 表名
     * @param columns 列定义列表
     *
     * ============================================================================
     *                          Compaction 流程图
     * ============================================================================
     *
     *   Compaction 触发条件：L0 文件大小 >= 64KB
     *
     *   Before Compaction:
     *   +-----------------------------------------------------------------+
     *   | L0 (newer)                                                      |
     *   | +-----+-----+-----+-----+-----+                                 |
     *   | |K1:V1|K2:V2|K3:V3|K5:V5|K7:V7|  (newest writes & deletions)    |
     *   | +-----+-----+-----+-----+-----+                                 |
     *   +-----------------------------------------------------------------+
     *                                | Merge
     *                                v
     *   +-----------------------------------------------------------------+
     *   | L1 (older)                                                      |
     *   | +-----+-----+-----+-----+-----+-----+                           |
     *   | |K1:X1|K2:X2|K4:V4|K5:X5|K6:V6|K8:V8|  (historical data)        |
     *   | +-----+-----+-----+-----+-----+-----+                           |
     *   +-----------------------------------------------------------------+
     *
     *   合并规则：
     *   - 相同Key：L0（较新）覆盖 L1（较旧）
     *   - 墓碑记录：values.empty() 表示删除
     *   - 按Key排序输出
     *
     *   After Compaction:
     *   +-----------------------------------------------------------------+
     *   | L0 (cleared)                                                    |
     *   | +---------------------------------------------------------+     |
     *   | | (empty)                                                 |     |
     *   | +---------------------------------------------------------+     |
     *   +-----------------------------------------------------------------+
     *
     *   +-----------------------------------------------------------------+
     *   | L1 (merged)                                                     |
     *   | +-----+-----+-----+-----+-----+-----+-----+-----+               |
     *   | |K1:V1|K2:V2|K3:V3|K4:V4|K5:V5|K6:V6|K7:V7|K8:V8| (sorted)      |
     *   | +-----+-----+-----+-----+-----+-----+-----+-----+               |
     *   +-----------------------------------------------------------------+
     *
     *   @par 资源消耗：
     *   虽然在后台执行，但大量磁盘I/O和CPU计算可能会影响前台写入性能。
     *
     *   时间复杂度：O(N log N)，其中N为合并的总行数
     *   空间复杂度：O(N)，需要在内存中构建合并映射
     */
    void LSMTreeEngine::compact_levels(const std::string& name, const std::vector<Column>& columns) {
        auto l0_path = base_path(name);
        if (!std::filesystem::exists(l0_path))
            return;

        const uintmax_t l0_threshold = Config::instance().l0_compaction_threshold_bytes();
        const int max_level = Config::instance().lsm_max_level();

        if (std::filesystem::file_size(l0_path) < l0_threshold)
            return;

        // Guard against concurrent compaction of the same table.
        auto* state = get_or_create_state(name);
        std::unique_lock comp_lock(state->compaction_mutex, std::try_to_lock);
        if (!comp_lock)
            return; // Another compaction is already running for this table.

        // GC horizon: versions with commit_ts <= gc_horizon that are not the
        // newest visible version for a pk can be safely discarded.
        const uint64_t gc_horizon = gc_horizon_fn_ ? gc_horizon_fn_() : 0;

        int level = 0;
        while (level <= max_level) {
            auto curr_path = (level == 0) ? base_path(name) : level_path(name, level);
            auto next_path = level_path(name, level + 1);

            if (!std::filesystem::exists(curr_path))
                break;

            auto curr_ptr = read_sstable(curr_path, columns);
            std::vector<MemEntry> curr_entries(curr_ptr->begin(), curr_ptr->end());
            std::vector<MemEntry> next_entries;
            if (std::filesystem::exists(next_path)) {
                auto next_ptr = read_sstable(next_path, columns);
                next_entries.assign(next_ptr->begin(), next_ptr->end());
            }

            // 多版本合并：两个流都按 (pk asc, ts desc) 排序，按 MVCCKey 合并保留所有版本。
            MVCCKeyCompare cmp;
            std::vector<MemEntry> merged_vec;
            merged_vec.reserve(curr_entries.size() + next_entries.size());
            auto it_curr = curr_entries.begin();
            auto it_next = next_entries.begin();
            while (it_curr != curr_entries.end() || it_next != next_entries.end()) {
                if (it_next == next_entries.end()) {
                    merged_vec.push_back(std::move(*it_curr++));
                } else if (it_curr == curr_entries.end()) {
                    merged_vec.push_back(std::move(*it_next++));
                } else {
                    MVCCKey ck{ it_curr->pk, it_curr->commit_ts };
                    MVCCKey nk{ it_next->pk, it_next->commit_ts };
                    if (cmp(ck, nk)) {
                        merged_vec.push_back(std::move(*it_curr++));
                    } else if (cmp(nk, ck)) {
                        merged_vec.push_back(std::move(*it_next++));
                    } else {
                        merged_vec.push_back(std::move(*it_curr++));
                        ++it_next;
                    }
                }
            }

            // GC: discard versions with commit_ts <= gc_horizon that are not
            // the newest visible version for their pk (runs at every level).
            std::vector<MemEntry> gc_vec;
            if (gc_horizon > 0) {
                gc_vec.reserve(merged_vec.size());
                size_t i = 0;
                while (i < merged_vec.size()) {
                    // 收集同 pk 段（相同 pk 在 merged_vec 中连续）。
                    size_t j = i;
                    while (j < merged_vec.size() && merged_vec[j].pk == merged_vec[i].pk)
                        ++j;

                    // 段内按 (ts desc) 排序：找到第一个 ts <= gc_horizon 的位置作为 anchor。
                    size_t anchor = j; // 默认无 anchor
                    for (size_t k = i; k < j; ++k) {
                        if (merged_vec[k].commit_ts <= gc_horizon) {
                            anchor = k;
                            break;
                        }
                    }

                    if (anchor == j) {
                        // 全部 ts > gc_horizon：保留所有版本（对未来的快照可见）。
                        for (size_t k = i; k < j; ++k)
                            gc_vec.push_back(std::move(merged_vec[k]));
                    } else {
                        const bool anchor_is_tombstone = merged_vec[anchor].tombstone;
                        const bool has_newer = (anchor > i);

                        if (anchor_is_tombstone && !has_newer) {
                            // 孤立 tombstone：无更新版本，已不再被需要，整段丢弃。
                        } else {
                            // 保留所有 ts > gc_horizon（k < anchor）和 anchor 本身；
                            // 丢弃 anchor 之前更旧的版本（k > anchor）。
                            for (size_t k = i; k <= anchor; ++k) {
                                gc_vec.push_back(std::move(merged_vec[k]));
                            }
                        }
                    }
                    i = j;
                }
                merged_vec = std::move(gc_vec);
            }

            write_sstable(next_path, columns, merged_vec);

            if (level == 0) {
                // L0 必须保留以满足 table_exists 检查
                write_sstable(curr_path, columns, std::vector<MemEntry>{});
            } else {
                std::filesystem::remove(curr_path);
            }

            // 级联：仅当下一层超过相应阈值才继续（阈值随层级倍增）。
            if (level + 1 >= max_level)
                break;
            if (!std::filesystem::exists(next_path))
                break;
            const uintmax_t threshold = l0_threshold << (level + 1);
            if (std::filesystem::file_size(next_path) < threshold)
                break;
            ++level;
        }
    }

    void LSMTreeEngine::checkpoint() {
        // Collect all table names from the states map.
        std::vector<std::string> tables;
        {
            std::shared_lock lock(states_mutex_);
            for (const auto& [name, _]: states_) {
                tables.push_back(name);
            }
        }

        for (const auto& name: tables) {
            // Load state if needed, then flush memtable if non-empty.
            auto* state = get_or_create_state(name);
            std::map<MVCCKey, MemEntry, MVCCKeyCompare> snapshot;
            {
                std::unique_lock lock(state->mutex);
                if (state->memtable.empty())
                    continue;
                snapshot = std::move(state->memtable);
                state->memtable.clear();
                state->memtable_bytes = 0;
            }
            flush_memtable_internal(name, state->schema, std::move(snapshot));
        }

        // Compact all tables through all levels.
        for (const auto& name: tables) {
            auto* state = get_or_create_state(name);
            // Run compaction sequentially (not via thread pool) so we know when it's done.
            compact_levels(name, state->schema);
        }

        // Truncate all WALs.
        for (const auto& name: tables) {
            WalManager::instance().truncate(wal_path(name));
        }
    }

} // namespace corodb
