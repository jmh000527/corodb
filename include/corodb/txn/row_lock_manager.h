// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file row_lock_manager.h @brief 行级写意向锁管理器。 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace corodb {

    /** @brief 线程安全的行级写意向锁管理器。 */
    class RowLockManager {
    public:
        RowLockManager() = default;
        RowLockManager(const RowLockManager&) = delete;
        RowLockManager& operator=(const RowLockManager&) = delete;

        /** @brief 尝试为 `(table, pk)` 申请写锁。 */
        std::optional<uint64_t> try_acquire(const std::string& table, int64_t pk, uint64_t txn_id);

        /** @brief 释放某个事务持有的全部行锁。 */
        void release_all(uint64_t txn_id);

        /** @brief 返回当前持有的行锁数。 */
        std::size_t held_count() const;

    private:
        mutable std::mutex mu_;
        std::unordered_map<std::string, std::unordered_map<int64_t, uint64_t>> by_row_;
        std::unordered_map<uint64_t, std::vector<std::pair<std::string, int64_t>>> by_txn_;
    };

} // namespace corodb
