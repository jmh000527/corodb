// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file lock_manager.cpp
// @brief 表级读写锁管理器的实现。

#include "corodb/txn/lock_manager.h"

#include <algorithm>

namespace corodb {

    // ---- LockManager --------------------------------------------------------

    /**
     * @brief 以共享模式加锁指定表（允许多读并发）。
     */
    void LockManager::lock_table_shared(const std::string& table_name) {
        get_or_create_table_lock(table_name).lock_shared();
    }

    /**
     * @brief 释放指定表的共享锁。
     */
    void LockManager::unlock_table_shared(const std::string& table_name) {
        if (auto* lock = get_table_lock(table_name))
            lock->unlock_shared();
    }

    /**
     * @brief 以独占模式加锁指定表（写操作使用）。
     */
    void LockManager::lock_table_exclusive(const std::string& table_name) {
        get_or_create_table_lock(table_name).lock();
    }

    /**
     * @brief 释放指定表的独占锁。
     */
    void LockManager::unlock_table_exclusive(const std::string& table_name) {
        if (auto* lock = get_table_lock(table_name))
            lock->unlock();
    }

    void LockManager::lock_table_exclusive_for(const std::string& table_name, std::chrono::milliseconds timeout) {
        auto& tl = get_or_create_table_lock(table_name);
        if (!tl.try_lock_for(timeout)) {
            throw std::runtime_error("[LockManager] Lock timeout: exclusive lock on '" + table_name + "'");
        }
    }

    void LockManager::lock_table_shared_for(const std::string& table_name, std::chrono::milliseconds timeout) {
        auto& tl = get_or_create_table_lock(table_name);
        if (!tl.try_lock_shared_for(timeout)) {
            throw std::runtime_error("[LockManager] Lock timeout: shared lock on '" + table_name + "'");
        }
    }

    /**
     * @brief 以独占模式加全局锁（DDL 等全库操作使用）。
     */
    void LockManager::lock_global_exclusive() {
        global_lock_.lock();
    }

    /**
     * @brief 释放全局独占锁。
     */
    void LockManager::unlock_global_exclusive() {
        global_lock_.unlock();
    }

    /**
     * @brief 以共享模式加全局锁（并发读操作使用）。
     */
    void LockManager::lock_global_shared() {
        global_lock_.lock_shared();
    }

    /**
     * @brief 释放全局共享锁。
     */
    void LockManager::unlock_global_shared() {
        global_lock_.unlock_shared();
    }

    /**
     * @brief 从分片映射中删除指定表的锁对象（表被删除后调用）。
     */
    void LockManager::remove_table_lock(const std::string& table_name) {
        const std::size_t shard = get_shard(table_name);
        std::unique_lock lock(shard_mutexes_[shard]);
        table_locks_[shard].erase(table_name);
    }

    /**
     * @brief 按表名哈希计算分片索引。
     */
    std::size_t LockManager::get_shard(const std::string& table_name) const noexcept {
        return std::hash<std::string>{}(table_name) % kNumShards;
    }

    /**
     * @brief 获取或创建指定表的 TableLock（线程安全）。
     */
    TableLock& LockManager::get_or_create_table_lock(const std::string& table_name) {
        const std::size_t shard = get_shard(table_name);
        std::unique_lock lock(shard_mutexes_[shard]);
        auto& map = table_locks_[shard];
        auto it = map.find(table_name);
        if (it == map.end())
            it = map.emplace(table_name, std::make_unique<TableLock>()).first;
        return *it->second;
    }

    /**
     * @brief 获取指定表的 TableLock 指针，不存在时返回 nullptr（noexcept）。
     */
    TableLock* LockManager::get_table_lock(const std::string& table_name) noexcept {
        const std::size_t shard = get_shard(table_name);
        std::shared_lock lock(shard_mutexes_[shard]);
        auto& map = table_locks_[shard];
        auto it = map.find(table_name);
        return it != map.end() ? it->second.get() : nullptr;
    }

    // ---- TableLockGuard -----------------------------------------------------

    /**
     * @brief 构造函数：按 mode 对指定表加共享或独占锁（RAII，析构时自动解锁）。
     */
    TableLockGuard::TableLockGuard(LockManager& manager, const std::string& table_name, LockMode mode)
        : manager_(manager), table_name_(table_name), mode_(mode) {
        if (mode_ == LockMode::Shared)
            manager_.lock_table_shared(table_name_);
        else
            manager_.lock_table_exclusive(table_name_);
    }

    /**
     * @brief 析构函数：释放表锁。
     */
    TableLockGuard::~TableLockGuard() {
        if (mode_ == LockMode::Shared)
            manager_.unlock_table_shared(table_name_);
        else
            manager_.unlock_table_exclusive(table_name_);
    }

    // ---- GlobalLockGuard ----------------------------------------------------

    /**
     * @brief 构造函数：按 mode 加全局共享或独占锁（RAII，析构时自动解锁）。
     */
    GlobalLockGuard::GlobalLockGuard(LockManager& manager, LockMode mode) : manager_(manager), mode_(mode) {
        if (mode_ == LockMode::Shared)
            manager_.lock_global_shared();
        else
            manager_.lock_global_exclusive();
    }

    /**
     * @brief 析构函数：释放全局锁。
     */
    GlobalLockGuard::~GlobalLockGuard() {
        if (mode_ == LockMode::Shared)
            manager_.unlock_global_shared();
        else
            manager_.unlock_global_exclusive();
    }

    // ---- MultiTableLockGuard ------------------------------------------------

    /**
     * @brief 构造函数：按字典序对多个表加锁（避免死锁），RAII 析构时逆序解锁。
     */
    MultiTableLockGuard::MultiTableLockGuard(LockManager& manager, std::vector<std::string> table_names, LockMode mode)
        : manager_(manager), table_names_(std::move(table_names)), mode_(mode) {
        // 按字典序加锁，避免死锁。
        std::sort(table_names_.begin(), table_names_.end());
        for (const auto& name: table_names_) {
            if (mode_ == LockMode::Shared)
                manager_.lock_table_shared(name);
            else
                manager_.lock_table_exclusive(name);
        }
    }

    /**
     * @brief 析构函数：逆序释放所有表锁。
     */
    MultiTableLockGuard::~MultiTableLockGuard() {
        for (auto it = table_names_.rbegin(); it != table_names_.rend(); ++it) {
            if (mode_ == LockMode::Shared)
                manager_.unlock_table_shared(*it);
            else
                manager_.unlock_table_exclusive(*it);
        }
    }

} // namespace corodb
