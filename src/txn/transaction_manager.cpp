// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file transaction_manager.cpp
// @brief 事务管理器的实现。

#include "corodb/txn/transaction_manager.h"

#include <limits>

namespace corodb {

    /**
     * @brief 开启新事务：分配全局唯一 txn_id 及读时间戳，注册为活跃事务。
     * @return 新分配的事务 ID。
     */
    uint64_t TransactionManager::begin() {
        const uint64_t id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t ts = next_ts_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(mutex_);
        txns_.emplace(id, TxnRecord{ id, ts, 0, TxnState::Active });
        return id;
    }

    /**
     * @brief 提交事务（不输出 commit_ts）。
     */
    bool TransactionManager::commit(uint64_t txn_id) {
        return commit(txn_id, nullptr);
    }

    /**
     * @brief 提交事务并分配全局 commit_ts。
     * @param out_commit_ts 若非空，接收分配到的 commit_ts。
     * @return 提交成功返回 true；事务不存在或状态非 Active 返回 false。
     */
    bool TransactionManager::commit(uint64_t txn_id, uint64_t* out_commit_ts) {
        std::unique_lock lock(mutex_);
        auto it = txns_.find(txn_id);
        if (it == txns_.end() || it->second.state != TxnState::Active) {
            return false;
        }
        const uint64_t ts = next_ts_.fetch_add(1, std::memory_order_relaxed);
        it->second.commit_ts = ts;
        it->second.state = TxnState::Committed;
        if (out_commit_ts)
            *out_commit_ts = ts;
        // Retain the committed record for MVCC visibility queries.
        // Pruned later by prune_committed() when safe to discard.
        committed_.emplace(txn_id, it->second);
        txns_.erase(it);
        return true;
    }

    /**
     * @brief 回滚事务：从活跃事务表中删除对应记录。
     * @return 成功返回 true；事务不存在或状态不可回滚返回 false。
     */
    bool TransactionManager::rollback(uint64_t txn_id) {
        std::unique_lock lock(mutex_);
        auto it = txns_.find(txn_id);
        if (it == txns_.end())
            return false;
        if (it->second.state != TxnState::Active && it->second.state != TxnState::Failed) {
            return false;
        }
        txns_.erase(it);
        return true;
    }

    /**
     * @brief 将 Active 状态的事务标记为 Failed，后续只能回滚。
     */
    void TransactionManager::mark_failed(uint64_t txn_id) {
        std::unique_lock lock(mutex_);
        auto it = txns_.find(txn_id);
        if (it != txns_.end() && it->second.state == TxnState::Active) {
            it->second.state = TxnState::Failed;
        }
    }

    /**
     * @brief 查询事务状态。
     * @return 事务状态；事务不存在时返回 nullopt。
     */
    std::optional<TxnState> TransactionManager::get_state(uint64_t txn_id) const {
        std::shared_lock lock(mutex_);
        auto it = txns_.find(txn_id);
        if (it != txns_.end())
            return it->second.state;
        auto jt = committed_.find(txn_id);
        if (jt != committed_.end())
            return jt->second.state;
        return std::nullopt;
    }

    /**
     * @brief 返回当前活跃（含 Failed）事务的数量。
     */
    size_t TransactionManager::active_count() const {
        std::shared_lock lock(mutex_);
        return txns_.size();
    }

    /**
     * @brief 分配一个新的全局时间戳（单调递增）。
     * @return 新时间戳值。
     */
    uint64_t TransactionManager::allocate_ts() {
        return next_ts_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief 查询指定事务的读时间戳（事务开始时分配）。
     * @return 读时间戳；事务不存在返回 nullopt。
     */
    std::optional<uint64_t> TransactionManager::get_read_ts(uint64_t txn_id) const {
        std::shared_lock lock(mutex_);
        auto it = txns_.find(txn_id);
        if (it != txns_.end())
            return it->second.read_ts;
        auto jt = committed_.find(txn_id);
        if (jt != committed_.end())
            return jt->second.read_ts;
        return std::nullopt;
    }

    /**
     * @brief 计算 GC 安全水位 = min(所有活跃事务的 read_ts)。
     *
     * MVCC 垃圾回收的核心：任何 commit_ts < min_active_read_ts 的旧版本
     * 都不可能被任何活跃事务看到，可以安全丢弃。
     *
     * 原理：
     * - 每个事务 BEGIN 时分配一个 read_ts（快照时间戳）
     * - 事务只读取 commit_ts <= 自己 read_ts 的数据版本
     * - 因此 commit_ts < 最老活跃事务的 read_ts 的版本，全局不可见
     *
     * 当无活跃事务时返回 next_ts_（当前最大时间戳），意味着一切皆可 GC。
     *
     * 复杂度：O(N) 活跃事务数。线程安全（共享锁）。
     */
    uint64_t TransactionManager::min_active_read_ts() const {
        std::shared_lock lock(mutex_);
        uint64_t min_ts = std::numeric_limits<uint64_t>::max();
        bool found = false;
        for (const auto& [id, rec]: txns_) {
            if (rec.state != TxnState::Active && rec.state != TxnState::Failed)
                continue;
            if (rec.read_ts < min_ts) {
                min_ts = rec.read_ts;
                found = true;
            }
        }
        if (!found)
            return next_ts_.load(std::memory_order_relaxed);
        return min_ts;
    }

    /**
     * @brief 恢复时提升时间戳下界，确保 next_ts_ 不低于 min_next_ts（单调提升，不降低）。
     * @param min_next_ts 从持久化层读取的最大 commit_ts + 1。
     */
    /**
     * @brief 查询已提交事务的 commit_ts。
     */
    std::optional<uint64_t> TransactionManager::get_commit_ts(uint64_t txn_id) const {
        std::shared_lock lock(mutex_);
        auto it = committed_.find(txn_id);
        if (it != committed_.end())
            return it->second.commit_ts;
        return std::nullopt;
    }

    /**
     * @brief 裁剪 committed_ 中所有 commit_ts < min_active_read_ts() 的记录。
     *
     * 没有活跃事务能再看到这些版本，保留它们只会浪费内存。
     */
    void TransactionManager::prune_committed() {
        const uint64_t horizon = min_active_read_ts();
        std::unique_lock lock(mutex_);
        for (auto it = committed_.begin(); it != committed_.end();) {
            if (it->second.commit_ts < horizon) {
                it = committed_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void TransactionManager::bootstrap_min_next_ts(uint64_t min_next_ts) {
        // CAS 单调提升：only raise, never lower.
        uint64_t cur = next_ts_.load(std::memory_order_relaxed);
        while (cur < min_next_ts && !next_ts_.compare_exchange_weak(cur, min_next_ts, std::memory_order_acq_rel,
                                                                    std::memory_order_relaxed)) {
            // retry; cur 已被 compare_exchange 更新为最新值
        }
    }

    /**
     * @brief 将 TxnState 枚举转换为可读字符串。
     */
    const char* to_string(TxnState s) noexcept {
        switch (s) {
            case TxnState::Active:
                return "Active";
            case TxnState::Failed:
                return "Failed";
            case TxnState::Committed:
                return "Committed";
            case TxnState::Aborted:
                return "Aborted";
        }
        return "Unknown";
    }

} // namespace corodb
