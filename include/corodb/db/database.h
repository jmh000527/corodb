// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file database.h @brief Database 门面类型定义。 */

#pragma once

#include <atomic>
#include <generator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "corodb/db/session.h"
#include "corodb/plan/physical_plan.h"
#include "corodb/sql/parser.h"
#include "corodb/storage/storage_engine_base.h"
#include "corodb/storage/table.h"
#include "corodb/txn/lock_manager.h"
#include "corodb/txn/row_lock_manager.h"
#include "corodb/txn/transaction_manager.h"

namespace corodb {

    class QueryProcessor;

    /** @brief 用户凭据管理器（SHA-256 密码哈希）。 */
    class UserManager {
    public:
        /** @brief 添加或更新用户。 */
        void add_user(const std::string& username, const std::string& password);

        /** @brief 验证凭据，密码匹配返回 true。 */
        [[nodiscard]] bool authenticate(const std::string& username, const std::string& password) const;

        /** @brief 是否已存在用户。 */
        [[nodiscard]] bool has_users() const noexcept { return !users_.empty(); }

    private:
        [[nodiscard]] static std::string hash_password(const std::string& password);

        std::unordered_map<std::string, std::string> users_; ///< username → hash
    };

    /** @brief 协调解析、优化、执行和存储的数据库入口。 */
    class Database {
    public:
        /** @brief 创建数据库实例并加载已有表。 */
        explicit Database(const std::string& data_dir);

        ~Database();

        // 禁止复制
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // 允许移动
        Database(Database&&) noexcept = default;
        Database& operator=(Database&&) noexcept = default;

        /** @brief 查询结果载荷。 */
        struct QueryResult {
            std::optional<std::string> message;
            std::optional<std::generator<Record>> rows;
            std::shared_ptr<void> plan;
            bool is_select{ false };

            [[nodiscard]] bool is_message() const noexcept;
            [[nodiscard]] bool is_success() const noexcept;
        };

        /** @brief 执行 SQL，并返回消息或惰性结果流。 */
        QueryResult execute(const std::string& sql);

        /** @brief 使用调用方提供的会话对象执行 SQL。 */
        QueryResult execute(const std::string& sql, std::shared_ptr<Session> session);

        [[nodiscard]] StorageEngine* get_storage() const noexcept {
            return storage_.get();
        }

        /// 获取表目录（含所有已注册表）。
        [[nodiscard]] Catalog& get_catalog() noexcept { return catalog_; }
        /// 获取表目录（只读）。
        [[nodiscard]] const Catalog& get_catalog() const noexcept { return catalog_; }

        /// 获取表级锁管理器（32 路分片 shared_mutex）。
        [[nodiscard]] LockManager& get_lock_manager() noexcept { return lock_manager_; }

        /// 获取行级锁管理器（first-committer-wins 写写冲突检测）。
        [[nodiscard]] RowLockManager& get_row_locks() noexcept { return row_locks_; }

        /// 获取事务管理器（时间戳分配 + 活跃事务追踪）。
        [[nodiscard]] TransactionManager& get_txn_manager() noexcept { return txn_manager_; }

        /// 获取用户凭据管理器（SHA-256 密码哈希）。
        [[nodiscard]] UserManager& get_user_manager() noexcept { return user_manager_; }
        /// 获取用户凭据管理器（只读）。
        [[nodiscard]] const UserManager& get_user_manager() const noexcept { return user_manager_; }

    private:
        std::unique_ptr<StorageEngine> storage_;
        Catalog catalog_;
        LockManager lock_manager_;
        TransactionManager txn_manager_;
        std::mutex commit_apply_mutex_;
        RowLockManager row_locks_;
        std::shared_ptr<Session> default_session_{ std::make_shared<Session>() };
        std::unique_ptr<QueryProcessor> query_processor_;
        UserManager user_manager_;
    };

} // namespace corodb
