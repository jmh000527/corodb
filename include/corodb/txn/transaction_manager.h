// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file transaction_manager.h @brief 事务状态与时间戳分配器。 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace corodb {

    /** @brief 事务状态机。 */
    enum class TxnState : uint8_t { Active, Failed, Committed, Aborted };

    /** @brief 单个事务的运行时记录。 */
    struct TxnRecord {
        uint64_t txn_id{ 0 };
        uint64_t read_ts{ 0 };
        uint64_t commit_ts{ 0 };
        TxnState state{ TxnState::Active };
    };

    /** @brief 进程级事务管理器。 */
    class TransactionManager {
    public:
        TransactionManager() = default;

        TransactionManager(const TransactionManager&) = delete;
        TransactionManager& operator=(const TransactionManager&) = delete;

        /** @brief 开启新事务。 */
        uint64_t begin();

        /** @brief 提交事务并分配 commit_ts。 */
        bool commit(uint64_t txn_id);

        /** @brief 提交事务，并把 commit_ts 写回调用方。 */
        bool commit(uint64_t txn_id, uint64_t* out_commit_ts);

        /** @brief 回滚事务。 */
        bool rollback(uint64_t txn_id);

        /** @brief 将事务标记为失败。 */
        void mark_failed(uint64_t txn_id);

        /** @brief 查询事务状态。 */
        std::optional<TxnState> get_state(uint64_t txn_id) const;

        /** @brief 返回当前活跃事务数。 */
        size_t active_count() const;

        /** @brief 启动恢复时提升下一可分配时间戳。 */
        void bootstrap_min_next_ts(uint64_t min_next_ts);

        /** @brief 分配新的全局单调时间戳。 */
        uint64_t allocate_ts();

        /** @brief 查询事务的 read_ts。 */
        std::optional<uint64_t> get_read_ts(uint64_t txn_id) const;

        /** @brief 返回可安全用于版本 GC 的最小活跃 read_ts。 */
        uint64_t min_active_read_ts() const;

        /** @brief 获取指定事务的 commit_ts（含已提交事务）。 */
        std::optional<uint64_t> get_commit_ts(uint64_t txn_id) const;

        /** @brief 裁剪已提交事务记录（调用时机：min_active_read_ts 变化时）。 */
        void prune_committed();

    private:
        mutable std::shared_mutex mutex_;
        std::atomic<uint64_t> next_txn_id_{ 1 };
        std::atomic<uint64_t> next_ts_{ 1 };
        std::unordered_map<uint64_t, TxnRecord> txns_;
        std::unordered_map<uint64_t, TxnRecord> committed_; ///< 已提交的事务（GC 水位以上保留）
    };

    /** @brief 将事务状态转为字符串。 */
    const char* to_string(TxnState s) noexcept;

} // namespace corodb
