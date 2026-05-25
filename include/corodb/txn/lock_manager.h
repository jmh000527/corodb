// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file lock_manager.h @brief 表级与全局锁管理。 */

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace corodb {

    /** @brief 锁模式枚举。 */
    enum class LockMode {
        Shared,   ///< 共享锁（读锁）
        Exclusive ///< 独占锁（写锁）
    };

    /** @brief 表级读写锁，封装 std::shared_mutex（支持超时变体）。 */
    class TableLock {
    public:
        TableLock() = default;

        /// 获取共享锁（多读并发）。
        void lock_shared() { mutex_.lock_shared(); }
        /// 释放共享锁。
        void unlock_shared() { mutex_.unlock_shared(); }
        /// 获取独占锁（写操作）。
        void lock() { mutex_.lock(); }
        /// 释放独占锁。
        void unlock() { mutex_.unlock(); }
        /// 尝试获取共享锁（非阻塞）。
        bool try_lock_shared() { return mutex_.try_lock_shared(); }
        /// 尝试获取独占锁（非阻塞）。
        bool try_lock() { return mutex_.try_lock(); }

        /** @brief 以独占模式加锁，超时返回 false。 */
        bool try_lock_for(std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!mutex_.try_lock()) {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        /** @brief 以共享模式加锁，超时返回 false。 */
        bool try_lock_shared_for(std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!mutex_.try_lock_shared()) {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

    private:
        std::shared_mutex mutex_;
    };

    /** @brief 全局锁管理器，管理所有表锁并提供统一的获取与释放接口。 */
    class LockManager {
    public:
        static constexpr std::size_t kNumShards = 32; ///< 内部分片数

        LockManager() = default;

        static constexpr std::chrono::milliseconds kDefaultLockTimeout{ 5000 };

        void lock_table_shared(const std::string& table_name);
        void unlock_table_shared(const std::string& table_name);
        void lock_table_exclusive(const std::string& table_name);
        void unlock_table_exclusive(const std::string& table_name);

        /** @brief 以独占模式加锁指定表，超时抛出 std::runtime_error。 */
        void lock_table_exclusive_for(const std::string& table_name, std::chrono::milliseconds timeout);
        /** @brief 以共享模式加锁指定表，超时抛出 std::runtime_error。 */
        void lock_table_shared_for(const std::string& table_name, std::chrono::milliseconds timeout);

        /** @brief 获取全局独占锁（用于 DDL 操作）。 */
        void lock_global_exclusive();
        void unlock_global_exclusive();

        /** @brief 获取全局共享锁（用于 DML 操作）。 */
        void lock_global_shared();
        void unlock_global_shared();

        /** @brief 删除表的锁。 */
        void remove_table_lock(const std::string& table_name);

    private:
        std::size_t get_shard(const std::string& table_name) const noexcept;
        TableLock& get_or_create_table_lock(const std::string& table_name);
        TableLock* get_table_lock(const std::string& table_name) noexcept;

        std::array<std::unordered_map<std::string, std::unique_ptr<TableLock>>, kNumShards> table_locks_;
        std::array<std::shared_mutex, kNumShards> shard_mutexes_;
        std::shared_mutex global_lock_;
    };

    /** @brief RAII 风格的表锁持有者，作用域结束时自动释放。 */
    class TableLockGuard {
    public:
        TableLockGuard(LockManager& manager, const std::string& table_name, LockMode mode);
        ~TableLockGuard();

        TableLockGuard(const TableLockGuard&) = delete;
        TableLockGuard& operator=(const TableLockGuard&) = delete;
        TableLockGuard(TableLockGuard&&) = delete;
        TableLockGuard& operator=(TableLockGuard&&) = delete;

    private:
        LockManager& manager_;
        std::string table_name_;
        LockMode mode_;
    };

    /** @brief RAII 风格的全局锁持有者。 */
    class GlobalLockGuard {
    public:
        GlobalLockGuard(LockManager& manager, LockMode mode);
        ~GlobalLockGuard();

        GlobalLockGuard(const GlobalLockGuard&) = delete;
        GlobalLockGuard& operator=(const GlobalLockGuard&) = delete;
        GlobalLockGuard(GlobalLockGuard&&) = delete;
        GlobalLockGuard& operator=(GlobalLockGuard&&) = delete;

    private:
        LockManager& manager_;
        LockMode mode_;
    };

    /** @brief 多表锁持有者，按字典序加锁以避免死锁。 */
    class MultiTableLockGuard {
    public:
        MultiTableLockGuard(LockManager& manager, std::vector<std::string> table_names, LockMode mode);
        ~MultiTableLockGuard();

        MultiTableLockGuard(const MultiTableLockGuard&) = delete;
        MultiTableLockGuard& operator=(const MultiTableLockGuard&) = delete;
        MultiTableLockGuard(MultiTableLockGuard&&) = delete;
        MultiTableLockGuard& operator=(MultiTableLockGuard&&) = delete;

    private:
        LockManager& manager_;
        std::vector<std::string> table_names_;
        LockMode mode_;
    };

} // namespace corodb
