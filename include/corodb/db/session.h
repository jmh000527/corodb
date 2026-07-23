// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file session.h @brief 每连接一份的会话状态。 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "corodb/storage/table.h"

namespace corodb {

    struct PlanNode;

    /** @brief SQL 标准的四种事务隔离级别。 */
    enum class IsolationLevel { ReadUncommitted, ReadCommitted, RepeatableRead, Serializable };

    /** @brief 单表的事务私有写缓冲。 */
    struct TableTxnBuffer {
        std::unordered_map<Value, Row, ValueHash, ValueEq> upserts;
        std::unordered_set<Value, ValueHash, ValueEq> deletes;

        bool empty() const noexcept {
            return upserts.empty() && deletes.empty();
        }
        void clear() noexcept {
            upserts.clear();
            deletes.clear();
        }
    };

    /** @brief 覆盖全部受影响表的事务写缓冲。 */
    struct TxnWriteBuffer {
        std::unordered_map<std::string, TableTxnBuffer> tables;

        TableTxnBuffer& for_table(const std::string& name) {
            return tables[name];
        }
        const TableTxnBuffer* find_table(const std::string& name) const {
            auto it = tables.find(name);
            return it == tables.end() ? nullptr : &it->second;
        }
        bool empty() const noexcept {
            return tables.empty();
        }
        void clear() noexcept {
            tables.clear();
        }
    };

    /** @brief 单个客户端连接的运行时状态（每连接一份，彼此隔离）。 */
    struct Session {
        uint64_t current_txn_id{ 0 };      ///< 当前活跃事务 ID（0 = 无事务）
        IsolationLevel isolation{ IsolationLevel::ReadCommitted }; ///< 当前隔离级别
        TxnWriteBuffer write_buffer;       ///< 事务私有写缓冲（COMMIT 时批量应用）
        uint64_t snapshot_ts{ 0 };         ///< 当前快照时间戳（MVCC 读取可见性）
        uint64_t auto_commit_ts{ 0 };      ///< 自动提交写操作的时间戳
        std::unordered_map<std::string, std::unordered_set<Value, ValueHash, ValueEq>> read_set; ///< Serializable 读集（PK 级）
        /// Serializable 幻读防护：记录各表在读取时的 write_version。
        std::unordered_map<std::string, uint64_t> table_read_versions;
        uint64_t statement_timeout_ms{ 0 }; ///< 0 = 无超时

        /// 预处理语句注册表：名称 → 缓存的物理计划。
        std::unordered_map<std::string, std::shared_ptr<PlanNode>> prepared_stmts;

        bool authenticated{ false }; ///< 是否已通过认证
        std::string auth_user;       ///< 认证用户名

        /// 是否处于活跃事务中。
        bool in_transaction() const noexcept { return current_txn_id != 0; }
    };

} // namespace corodb
