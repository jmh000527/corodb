// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file row_lock_manager.cpp
// @brief 行级锁管理器的实现。

#include "corodb/txn/row_lock_manager.h"

namespace corodb {

    /**
     * @brief 尝试对指定表行加写锁。
     * @param table 表名。
     * @param pk 行主键。
     * @param txn_id 请求锁的事务 ID。
     * @return 若获取成功返回 nullopt；若锁已被他人持有，返回当前持有者的 txn_id。
     */
    std::optional<uint64_t> RowLockManager::try_acquire(const std::string& table, int64_t pk, uint64_t txn_id) {
        std::scoped_lock lk(mu_);
        auto& row_map = by_row_[table];
        auto it = row_map.find(pk);
        if (it == row_map.end()) {
            row_map.emplace(pk, txn_id);
            by_txn_[txn_id].emplace_back(table, pk);
            return std::nullopt;
        }
        if (it->second == txn_id) {
            return std::nullopt; // 幂等：自己再次写同一 pk
        }
        return it->second; // 冲突：返回当前持有者
    }

    /**
     * @brief 释放指定事务持有的所有行锁。
     */
    void RowLockManager::release_all(uint64_t txn_id) {
        std::scoped_lock lk(mu_);
        auto it = by_txn_.find(txn_id);
        if (it == by_txn_.end())
            return;
        for (auto& [table, pk]: it->second) {
            auto map_it = by_row_.find(table);
            if (map_it == by_row_.end())
                continue;
            auto pk_it = map_it->second.find(pk);
            if (pk_it != map_it->second.end() && pk_it->second == txn_id) {
                map_it->second.erase(pk_it);
            }
            if (map_it->second.empty()) {
                by_row_.erase(map_it);
            }
        }
        by_txn_.erase(it);
    }

    /**
     * @brief 返回当前持有的行锁总数（跨所有表）。
     */
    std::size_t RowLockManager::held_count() const {
        std::scoped_lock lk(mu_);
        std::size_t n = 0;
        for (auto& [_, m]: by_row_)
            n += m.size();
        return n;
    }

} // namespace corodb
