// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file database.cpp
// @brief 数据库门面实现：仅持有资源，转发到 QueryProcessor。

#include "corodb/db/database.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

#include "corodb/common/logger.h"
#include "corodb/process/query_processor.h"
#include "corodb/storage/lsm_storage_engine.h"

namespace corodb {

    // ---- UserManager ----

    std::string UserManager::hash_password(const std::string& password) {
        // 简单加盐哈希。生产环境应使用 bcrypt / scrypt / Argon2。
        const std::string salt = Config::instance().auth_salt();
        std::string input = salt + password;
        // FNV-1a 64-bit 简单哈希（非加密安全）。
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : input) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        // 返回十六进制字符串。
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return std::string(buf);
    }

    void UserManager::add_user(const std::string& username, const std::string& password) {
        users_[username] = hash_password(password);
    }

    bool UserManager::authenticate(const std::string& username, const std::string& password) const {
        auto it = users_.find(username);
        if (it == users_.end())
            return false;
        return it->second == hash_password(password);
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
