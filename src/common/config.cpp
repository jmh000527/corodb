// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file config.cpp
// @brief 全局配置类的文件加载/导出实现。
//
// 配置文件采用 INI 风格的 key = value 格式：
//   - 以 `#` 或 `;` 起始的行视为注释；
//   - 支持 `[section]` 段头作为 key 的命名空间前缀（例如 `[storage]` 段下的 `memtable_size_bytes`
//     对应完整 key `storage.memtable_size_bytes`）；
//   - 也允许直接写 `storage.memtable_size_bytes = 1048576` 的扁平形式。
//
// 解析失败的行会被忽略并打印警告，但不会终止启动；未识别的 key 同样被忽略，
// 以便在添加新选项后旧配置文件仍可使用。

#include "corodb/common/config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace corodb {

    namespace {

        // 去除字符串两端空白
        std::string trim(std::string_view sv) {
            std::size_t b = 0;
            while (b < sv.size() && static_cast<unsigned char>(sv[b]) <= ' ')
                ++b;
            std::size_t e = sv.size();
            while (e > b && static_cast<unsigned char>(sv[e - 1]) <= ' ')
                --e;
            return std::string(sv.substr(b, e - b));
        }

        // 将字符串转换为小写
        std::string to_lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // 将字符串解析为整数，解析失败返回 nullopt
        std::optional<long long> parse_int(const std::string& v) {
            try {
                std::size_t pos = 0;
                long long n = std::stoll(v, &pos, 10);
                if (pos != v.size())
                    return std::nullopt;
                return n;
            } catch (...) {
                return std::nullopt;
            }
        }

    } // namespace

    /**
     * @brief 从 INI 风格配置文件加载配置项，更新当前 Config 对象。
     * @param path 配置文件路径。
     * @param error 若非空，在失败时接收错误描述。
     * @return 成功返回 true，文件无法打开返回 false。
     */
    bool Config::load_from_file(const std::string& path, std::string* error) {
        std::ifstream ifs(path);
        if (!ifs) {
            if (error)
                *error = "cannot open config file: " + path;
            return false;
        }

        std::string line;
        std::string section;
        int line_no = 0;
        while (std::getline(ifs, line)) {
            ++line_no;
            // 去除行内注释（# 之后）
            std::size_t hash = line.find_first_of("#;");
            if (hash != std::string::npos)
                line.resize(hash);
            std::string trimmed = trim(line);
            if (trimmed.empty())
                continue;

            // [section] 段头
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                section = trim(std::string_view{ trimmed }.substr(1, trimmed.size() - 2));
                continue;
            }

            std::size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = trim(std::string_view{ trimmed }.substr(0, eq));
            std::string val = trim(std::string_view{ trimmed }.substr(eq + 1));
            if (key.empty())
                continue;

            std::string full_key = section.empty() ? key : section + "." + key;
            apply_kv(full_key, val);
        }

        config_path_ = path;
        return true;
    }

    /**
     * @brief 将单个 key-value 配置项应用到 Config 对象的对应字段。
     * @param key 完整 key（如 "storage.page_size"）。
     * @param value 字符串形式的值。
     */
    void Config::apply_kv(const std::string& key, const std::string& value) {
        const std::string k = to_lower(key);

        auto try_set_size = [&](const char* name, std::size_t Config::* member) {
            if (k != name)
                return false;
            if (auto n = parse_int(value); n.has_value() && *n >= 0) {
                this->*member = static_cast<std::size_t>(*n);
            }
            return true;
        };
        auto try_set_u32 = [&](const char* name, uint32_t Config::* member) {
            if (k != name)
                return false;
            if (auto n = parse_int(value); n.has_value() && *n >= 0) {
                this->*member = static_cast<uint32_t>(*n);
            }
            return true;
        };
        auto try_set_u16 = [&](const char* name, uint16_t Config::* member) {
            if (k != name)
                return false;
            if (auto n = parse_int(value); n.has_value() && *n >= 1 && *n <= 65535) {
                this->*member = static_cast<uint16_t>(*n);
            }
            return true;
        };
        auto try_set_str = [&](const char* name, std::string Config::* member) {
            if (k != name)
                return false;
            this->*member = value;
            return true;
        };

        // storage.*
        if (try_set_size("storage.page_size", &Config::page_size_))
            return;
        if (try_set_size("storage.buffer_pages", &Config::buffer_pages_))
            return;
        if (try_set_size("storage.memtable_size_bytes", &Config::memtable_size_bytes_))
            return;

        // wal.*
        if (try_set_u32("wal.group_commit_delay_us", &Config::group_commit_delay_us_))
            return;
        if (try_set_u32("wal.group_commit_batch_size", &Config::group_commit_batch_size_))
            return;

        // server.*
        if (try_set_u16("server.port", &Config::server_port_)) return;
        if (try_set_str("server.data_dir", &Config::data_dir_)) return;
        if (try_set_size("server.max_connections", &Config::max_connections_)) return;
        if (try_set_size("server.io_threads", &Config::io_threads_)) return;
        if (try_set_size("server.worker_threads", &Config::worker_threads_)) return;
        if (k == "server.reuse_port") { reuse_port_ = (value == "true" || value == "1"); return; }
        if (try_set_size("server.idle_timeout_sec", &Config::idle_timeout_sec_)) return;
        if (try_set_size("server.statement_timeout_ms", &Config::statement_timeout_ms_)) return;

        // wal.*  (continued)
        if (try_set_str("wal.sync_mode", &Config::wal_sync_mode_)) return;

        // connection.*
        if (try_set_size("connection.max_buffer_size", &Config::max_buffer_size_)) return;

        // network.*
        if (try_set_u32("network.send_timeout_ms", &Config::net_send_timeout_ms_)) return;
        if (try_set_u32("network.read_timeout_ms", &Config::net_read_timeout_ms_)) return;

        // lock_manager.*
        if (try_set_u32("lock_manager.timeout_ms", &Config::lock_timeout_ms_)) return;

        // lsm.*
        if (try_set_size("lsm.l0_compaction_threshold_bytes", &Config::l0_threshold_)) return;
        if (k == "lsm.max_level") { auto n = parse_int(value); if (n && *n >= 1 && *n <= 7) { lsm_max_level_ = static_cast<int>(*n); } return; }
        if (try_set_size("lsm.sst_cache_entries", &Config::sst_cache_entries_)) return;

        // plan_cache.*
        if (try_set_size("plan_cache.max_entries", &Config::plan_cache_entries_)) return;

        // thread_pool.*
        if (try_set_size("thread_pool.max_queue_size", &Config::thread_pool_max_queue_)) return;

        // auth.*
        if (try_set_str("auth.password_salt", &Config::auth_salt_)) return;
    }

    /**
     * @brief 将默认配置写出为带注释的 INI 文件。
     * @param path 目标文件路径。
     * @return 写入成功返回 true。
     */
    bool Config::write_default_file(const std::string& path) {
        std::ofstream ofs(path);
        if (!ofs)
            return false;
        Config tmp;

        ofs << "# ============================================================================\n";
        ofs << "#  CoroDB 配置文件（INI 格式）\n";
        ofs << "# ============================================================================\n";
        ofs << "#\n";
        ofs << "#  语法：\n";
        ofs << "#    - 注释以 # 或 ; 开头\n";
        ofs << "#    - [section] 段头对后续 key 分组\n";
        ofs << "#    - key = value（等号两侧空格可选）\n";
        ofs << "#    - 也支持扁平写法：storage.page_size = 8192\n";
        ofs << "#\n";
        ofs << "#  生效方式：修改本文件后重启 corodb_server。\n";
        ofs << "#  未识别的 key 会被静默忽略（向前兼容）。\n";
        ofs << "#\n";
        ofs << "#  生成默认配置：corodb_genconfig [output_path]\n";
        ofs << "#\n\n";

        // ---- [storage] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  存储引擎参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[storage]\n";
        ofs << "# 数据页大小（字节），影响 Buffer Pool 和 SSTable 读写粒度，默认 8192。\n";
        ofs << "page_size = " << tmp.page_size_ << "\n";
        ofs << "\n";
        ofs << "# 缓冲池容量（页数），总内存占用 ≈ page_size × buffer_pages。\n";
        ofs << "buffer_pages = " << tmp.buffer_pages_ << "\n";
        ofs << "\n";
        ofs << "# MemTable 大小阈值（字节），达到后触发 flush 写入 L0 SSTable，默认 1 MB。\n";
        ofs << "memtable_size_bytes = " << tmp.memtable_size_bytes_ << "\n";
        ofs << "\n\n";

        // ---- [wal] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  预写日志（WAL）参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[wal]\n";
        ofs << "# 同步模式（默认 durable，保证断电不丢已提交数据）：\n";
        ofs << "#   fast    — 仅 C++ 运行时 flush 到 OS 缓存（高吞吐，断电可能丢失未落盘数据）\n";
        ofs << "#   durable — 内核级 fsync，确保数据写入物理介质（ACID 持久性）\n";
        ofs << "sync_mode = " << tmp.wal_sync_mode_ << "\n";
        ofs << "\n";
        ofs << "# Group Commit 参数：Leader-Follower 模式，将多次 fsync 合并为一次。\n";
        ofs << "#   delay_us  — 最长等待时间（微秒），超时后立即 fsync\n";
        ofs << "#   batch_size — 累积条数阈值，达到后立即 fsync\n";
        ofs << "# 设 delay_us=0 且 batch_size=1 可退化为每次提交立即 fsync。\n";
        ofs << "group_commit_delay_us = " << tmp.group_commit_delay_us_ << "\n";
        ofs << "group_commit_batch_size = " << tmp.group_commit_batch_size_ << "\n";
        ofs << "\n\n";

        // ---- [server] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  服务器参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[server]\n";
        ofs << "# TCP 监听端口（1–65535）。\n";
        ofs << "port = " << tmp.server_port_ << "\n";
        ofs << "\n";
        ofs << "# 数据文件存储目录（相对路径相对于 corodb_server 所在目录）。\n";
        ofs << "data_dir = " << tmp.data_dir_ << "\n";
        ofs << "\n";
        ofs << "# 最大并发连接数，超过后新连接被拒绝。\n";
        ofs << "max_connections = " << tmp.max_connections_ << "\n";
        ofs << "\n";
        ofs << "# I/O 线程数（Sub Reactor），0 = 自动检测 CPU 核心数。\n";
        ofs << "io_threads = " << tmp.io_threads_ << "\n";
        ofs << "\n";
        ofs << "# SQL 执行线程数（Worker Pool），0 = 自动检测 CPU 核心数。\n";
        ofs << "worker_threads = " << tmp.worker_threads_ << "\n";
        ofs << "\n";
        ofs << "# 端口复用（SO_REUSEPORT），允许多实例绑定同一端口。\n";
        ofs << "reuse_port = " << (tmp.reuse_port_ ? "true" : "false") << "\n";
        ofs << "\n";
        ofs << "# 空闲连接超时（秒），超时后自动断开，0 = 禁用。\n";
        ofs << "idle_timeout_sec = " << tmp.idle_timeout_sec_ << "\n";
        ofs << "\n";
        ofs << "# 单条 SQL 语句执行超时（毫秒），超时后抛出异常并中止，0 = 禁用。\n";
        ofs << "statement_timeout_ms = " << tmp.statement_timeout_ms_ << "\n";
        ofs << "\n\n";

        // ---- [connection] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  连接管理参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[connection]\n";
        ofs << "# 单个连接的收发缓冲区上限（字节），超过后强制断开，防止 OOM。\n";
        ofs << "max_buffer_size = " << tmp.max_buffer_size_ << "\n";
        ofs << "\n\n";

        // ---- [network] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  网络超时参数（影响 csql 客户端行为）\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[network]\n";
        ofs << "# 发送超时（毫秒）。\n";
        ofs << "send_timeout_ms = " << tmp.net_send_timeout_ms_ << "\n";
        ofs << "\n";
        ofs << "# 接收响应超时（毫秒）。\n";
        ofs << "read_timeout_ms = " << tmp.net_read_timeout_ms_ << "\n";
        ofs << "\n\n";

        // ---- [lock_manager] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  锁管理参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[lock_manager]\n";
        ofs << "# 表级锁等待超时（毫秒），超时后抛出 Lock timeout 异常。\n";
        ofs << "timeout_ms = " << tmp.lock_timeout_ms_ << "\n";
        ofs << "\n\n";

        // ---- [lsm] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  LSM-Tree 引擎参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[lsm]\n";
        ofs << "# L0 SSTable 大小阈值（字节），超过后触发后台 Compaction（L0→L1→L2→…）。\n";
        ofs << "l0_compaction_threshold_bytes = " << tmp.l0_threshold_ << "\n";
        ofs << "\n";
        ofs << "# LSM 最大层级数（1–7，含 L0），控制树深度和读放大。\n";
        ofs << "max_level = " << tmp.lsm_max_level_ << "\n";
        ofs << "\n";
        ofs << "# SSTable 解码缓存容量（条目数），LRU 淘汰。\n";
        ofs << "sst_cache_entries = " << tmp.sst_cache_entries_ << "\n";
        ofs << "\n\n";

        // ---- [plan_cache] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  计划缓存参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[plan_cache]\n";
        ofs << "# 物理计划 LRU 缓存容量（条目数），仅缓存 SELECT 语句。\n";
        ofs << "max_entries = " << tmp.plan_cache_entries_ << "\n";
        ofs << "\n\n";

        // ---- [thread_pool] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  线程池参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[thread_pool]\n";
        ofs << "# 任务队列最大长度（0 = 无限制）。超过后提交任务会阻塞调用者（背压）。\n";
        ofs << "max_queue_size = " << tmp.thread_pool_max_queue_ << "\n";
        ofs << "\n\n";

        // ---- [auth] ----
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "#  认证参数\n";
        ofs << "# ----------------------------------------------------------------------------\n";
        ofs << "[auth]\n";
        ofs << "# 密码哈希盐值，修改后所有已有密码失效（需重新 CREATE USER）。\n";
        ofs << "password_salt = " << tmp.auth_salt_ << "\n";

        return true;
    }

} // namespace corodb
