// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file database.cpp
// @brief 数据库门面实现：仅持有资源，转发到 QueryProcessor。

#include "corodb/db/database.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

#include "corodb/common/crypto.h"
#include "corodb/common/logger.h"
#include "corodb/process/query_processor.h"
#include "corodb/storage/lsm_storage_engine.h"

#include <random>
#include <string_view>

namespace corodb {

    // ---- UserManager ----

    std::string UserManager::hash_password(const std::string& password) {
        // PBKDF2-HMAC-SHA256：每用户独立随机盐（16 字节）+ 可配迭代次数（[auth].pbkdf2_iterations）。
        std::random_device rd;
        std::mt19937_64 rng{ (static_cast<uint64_t>(rd()) << 32) ^ rd() };
        std::string salt(16, '\0');
        for (auto& c: salt)
            c = static_cast<char>(rng() & 0xFF);
        const std::string dk =
                crypto::pbkdf2_hmac_sha256(password, salt, Config::instance().auth_pbkdf2_iterations(), 32);
        return "pbkdf2-sha256$" + std::to_string(Config::instance().auth_pbkdf2_iterations()) + "$" +
               crypto::to_hex(salt) + "$" + crypto::to_hex(dk);
    }

    std::string UserManager::legacy_hash_password(const std::string& password) {
        // 旧版 FNV-1a 64-bit（非加密安全）：仅为存量账号保留校验，登录后应重设口令迁移。
        const std::string salt = Config::instance().auth_salt();
        std::string input = salt + password;
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : input) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return std::string(buf);
    }

    bool UserManager::verify_password(const std::string& stored, const std::string& password) {
        // 格式分发：pbkdf2-sha256$iter$salt_hex$dk_hex | 旧版 16 位十六进制 FNV。
        constexpr std::string_view kPrefix = "pbkdf2-sha256$";
        if (stored.rfind(kPrefix, 0) != 0) {
            return crypto::constant_time_equal(stored, legacy_hash_password(password));
        }
        // 解析 iter$salt_hex$dk_hex。
        const std::string body = stored.substr(kPrefix.size());
        const auto p1 = body.find('$');
        if (p1 == std::string::npos)
            return false;
        const auto p2 = body.find('$', p1 + 1);
        if (p2 == std::string::npos)
            return false;
        uint32_t iterations = 0;
        try {
            const unsigned long long parsed = std::stoull(body.substr(0, p1));
            if (parsed == 0 || parsed > 10'000'000ull)
                return false;
            iterations = static_cast<uint32_t>(parsed);
        } catch (...) {
            return false;
        }
        const std::string salt_hex = body.substr(p1 + 1, p2 - p1 - 1);
        const std::string dk_hex = body.substr(p2 + 1);
        // 编码长度约定：salt 16 字节 → 32 hex；dk 32 字节 → 64 hex。
        if (salt_hex.size() != 32 || dk_hex.size() != 64)
            return false;
        auto hex_decode = [](const std::string& hex) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(hex.size() / 2);
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                const int hi = nibble(hex[i]);
                const int lo = nibble(hex[i + 1]);
                if (hi < 0 || lo < 0)
                    return std::string();
                out.push_back(static_cast<char>((hi << 4) | lo));
            }
            return out;
        };
        const std::string salt = hex_decode(salt_hex);
        if (salt.empty())
            return false;
        const std::string dk = crypto::pbkdf2_hmac_sha256(password, salt, iterations, 32);
        return crypto::constant_time_equal(crypto::to_hex(dk), dk_hex);
    }

    void UserManager::add_user(const std::string& username, const std::string& password) {
        users_[username] = hash_password(password);
    }

    bool UserManager::authenticate(const std::string& username, const std::string& password) const {
        auto it = users_.find(username);
        if (it == users_.end())
            return false;
        return verify_password(it->second, password);
    }

    /**
     * @brief 构造数据库实例：初始化 LSM 存储引擎，从磁盘重建 Catalog，恢复 commit_ts 水位。
     * @param data_dir 数据文件目录路径。
     */
    Database::Database(const std::string& data_dir) : storage_{ std::make_unique<LSMTreeEngine>(data_dir) } {
        auto* lsm = static_cast<LSMTreeEngine*>(storage_.get());
        lsm->set_gc_horizon([this] { return txn_manager_.min_active_read_ts(); });

        if (std::filesystem::exists(data_dir)) {
            std::set<std::string> loaded_tables;
            for (const auto& entry: std::filesystem::directory_iterator(data_dir)) {
                if (!entry.is_regular_file())
                    continue;

                const auto& p = entry.path();
                std::string filename = p.filename().string();
                auto pos = filename.rfind(".lsm.L");
                if (pos == std::string::npos)
                    continue;

                std::string suffix = filename.substr(pos + 6);
                bool is_num = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit);
                if (!is_num)
                    continue;

                std::string name = filename.substr(0, pos);
                if (!name.empty() && loaded_tables.find(name) == loaded_tables.end()) {
                    try {
                        catalog_.register_table(std::make_shared<Table>(name, std::vector<Column>{}, storage_.get()));
                        loaded_tables.insert(name);
                    } catch (const std::exception& ex) {
                        LOG_WARN("Skip table {}: {}", name, ex.what());
                    }
                }
            }
        }

        if (const uint64_t max_ts = storage_->max_observed_commit_ts(); max_ts > 0) {
            txn_manager_.bootstrap_min_next_ts(max_ts + 1);
        }

        query_processor_ = std::make_unique<QueryProcessor>(catalog_, *storage_, txn_manager_, lock_manager_,
                                                            row_locks_, commit_apply_mutex_, user_manager_);

        // Auth is opt-in: disabled until the first user is added.
    }

    Database::~Database() = default;

    /**
     * @brief 判断查询结果是否为纯文本消息（无行集）。
     */
    bool Database::QueryResult::is_message() const noexcept {
        return message.has_value() && !rows.has_value();
    }

    /**
     * @brief 判断查询是否成功（有行集，或消息不以 "ERROR" 开头）。
     */
    bool Database::QueryResult::is_success() const noexcept {
        return rows.has_value() || (message.has_value() && !message->starts_with("ERROR"));
    }

    /**
     * @brief 使用默认会话执行 SQL 语句。
     */
    Database::QueryResult Database::execute(const std::string& sql) {
        return execute(sql, std::shared_ptr<Session>(default_session_));
    }

    /**
     * @brief 在指定会话上下文中执行 SQL 语句。
     * @param session 当前事务会话。
     */
    Database::QueryResult Database::execute(const std::string& sql, std::shared_ptr<Session> session) {
        ProcessedQuery pq = query_processor_->run(sql, std::move(session));
        QueryResult qr;
        qr.message = std::move(pq.message);
        qr.rows = std::move(pq.rows);
        qr.plan = std::move(pq.plan);
        qr.is_select = pq.is_select;
        return qr;
    }

} // namespace corodb
