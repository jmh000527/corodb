// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file transaction_controller.cpp
// @brief 事务控制语句（BEGIN/COMMIT/ROLLBACK/SET TX）处理实现。

#include "corodb/process/transaction_controller.h"

#include <climits>
#include <stdexcept>
#include <unordered_map>

namespace corodb {

    /**
     * @brief 检查 stmt 类型并分发到 begin/commit/rollback/set_transaction 处理器。
     * @return 事务控制结果；若 stmt 不是事务控制语句则返回 nullopt。
     */
    std::optional<TxnControlResult> TransactionController::handle(const Statement& stmt, Session& session) {
        if (std::holds_alternative<BeginStmt>(stmt))
            return begin(session);
        if (std::holds_alternative<CommitStmt>(stmt))
            return commit(session);
        if (std::holds_alternative<RollbackStmt>(stmt)) {
            const auto& rb = std::get<RollbackStmt>(stmt);
            if (rb.savepoint.has_value())
                return rollback_to_savepoint(*rb.savepoint, session);
            return rollback(session);
        }
        if (std::holds_alternative<SavepointStmt>(stmt))
            return savepoint(std::get<SavepointStmt>(stmt).name, session);
        if (std::holds_alternative<ReleaseSavepointStmt>(stmt))
            return release_savepoint(std::get<ReleaseSavepointStmt>(stmt).name, session);
        if (std::holds_alternative<SetTransactionStmt>(stmt))
            return set_transaction(std::get<SetTransactionStmt>(stmt), session);
        return std::nullopt;
    }

    /**
     * @brief 开启新事务：分配 txn_id，并根据隔离级别设置 snapshot_ts。
     * @return 包含新事务 ID 的文本结果。
     */
    TxnControlResult TransactionController::begin(Session& session) {
        if (session.current_txn_id != 0) {
            throw std::runtime_error("[Process] Transaction already in progress");
        }
        uint64_t txn_id = txn_manager_.begin();
        (void)storage_.begin_transaction();
        session.current_txn_id = txn_id;
        session.savepoints.clear();
        if (session.isolation == IsolationLevel::RepeatableRead || session.isolation == IsolationLevel::Serializable) {
            auto rts = txn_manager_.get_read_ts(txn_id);
            session.snapshot_ts = rts.value_or(0);
        } else {
            session.snapshot_ts = 0;
        }
        return TxnControlResult{ "Transaction started with ID: " + std::to_string(txn_id) };
    }

    /**
     * @brief 提交事务：Serializable 下检测写-读冲突，应用写缓冲，刷盘，释放行锁。
     * @return 提交成功/失败的文本结果。
     */
    TxnControlResult TransactionController::commit(Session& session) {
        if (session.current_txn_id == 0) {
            throw std::runtime_error("[Process] No active transaction to commit");
        }
        uint64_t txn_id = session.current_txn_id;
        auto state = txn_manager_.get_state(txn_id);
        // 已标记失败的事务只能回滚
        if (state && *state == TxnState::Failed) {
            txn_manager_.rollback(txn_id);
            session.current_txn_id = 0;
            session.write_buffer.clear();
            session.read_set.clear();
        session.table_read_versions.clear();
            session.snapshot_ts = 0;
            row_locks_.release_all(txn_id);
            throw std::runtime_error("[Process] Cannot COMMIT failed transaction; rolled back instead");
        }

        uint64_t commit_ts = 0;
        try {
            // 串行化冲突检测 + 写缓冲应用必须在同一把锁下原子完成
            std::scoped_lock commit_guard(commit_apply_mutex_);

            // Serializable 级别：检查快照期间读集是否已被修改
            if (session.isolation == IsolationLevel::Serializable) {
                // 主键级冲突检测。
                for (const auto& [tname, pks]: session.read_set) {
                    auto tbl = catalog_.lookup(tname);
                    if (!tbl)
                        continue;
                    for (const Value& pk: pks) {
                        auto old_row = tbl->lookup_visible(pk, session.snapshot_ts);
                        auto cur_row = tbl->lookup_visible(pk, UINT64_MAX);
                        const bool changed =
                                old_row.has_value() != cur_row.has_value() ||
                                (old_row.has_value() && cur_row.has_value() && old_row->values != cur_row->values);
                        if (changed) {
                            txn_manager_.rollback(txn_id);
                            session.write_buffer.clear();
                            session.read_set.clear();
        session.table_read_versions.clear();
                            session.table_read_versions.clear();
                            session.current_txn_id = 0;
                            session.snapshot_ts = 0;
                            row_locks_.release_all(txn_id);
                            throw std::runtime_error("[Process] Serialization failure: row read by this "
                                                     "transaction was concurrently modified (table=" +
                                                     tname + ")");
                        }
                    }
                }

                // 表级幻读检测：若读取过的表被并发事务修改，中止当前事务。
                for (const auto& [tname, read_version] : session.table_read_versions) {
                    auto tbl = catalog_.lookup(tname);
                    if (!tbl)
                        continue;
                    if (tbl->write_counter() != read_version) {
                        txn_manager_.rollback(txn_id);
                        session.write_buffer.clear();
                        session.read_set.clear();
        session.table_read_versions.clear();
                        session.table_read_versions.clear();
                        session.current_txn_id = 0;
                        session.snapshot_ts = 0;
                        row_locks_.release_all(txn_id);
                        throw std::runtime_error("[Process] Serialization failure: phantom read detected "
                                                 "(table=" + tname + " was modified by concurrent transaction)");
                    }
                }
            }

            // 分配全局提交时间戳（commit_ts），向后快照可见
            bool ok_mgr = txn_manager_.commit(txn_id, &commit_ts);
            if (!ok_mgr) {
                throw std::runtime_error("[Process] TxnManager.commit failed (state mismatch)");
            }
            // 将写缓冲中的 upsert / delete 批量刷入持久化层
            for (auto& [tname, tbuf]: session.write_buffer.tables) {
                if (tbuf.empty())
                    continue;
                auto tbl = catalog_.lookup(tname);
                if (!tbl)
                    continue;

                if (tbl->has_storage()) {
                    // 存储型：直接持久化（去除 rows_ 全量常驻；读取走 scan_visible）。
                    for (const Value& k: tbuf.deletes) {
                        tbl->persist_row_delete(k, commit_ts);
                    }
                    for (auto& [pk, row]: tbuf.upserts) {
                        tbl->persist_row_upsert(row, commit_ts);
                    }
                } else {
                    // 纯内存表：将缓冲应用到 rows_。
                    auto& rows = tbl->rows_mut();
                    if (!tbuf.deletes.empty()) {
                        std::erase_if(rows, [&](const Row& row) -> bool {
                            if (row.values.empty())
                                return false;
                            return tbuf.deletes.count(tbl->row_key(row)) > 0;
                        });
                    }
                    if (!tbuf.upserts.empty()) {
                        std::unordered_map<Value, std::size_t, ValueHash, ValueEq> idx_by_pk;
                        idx_by_pk.reserve(rows.size());
                        for (std::size_t i = 0; i < rows.size(); ++i) {
                            if (!rows[i].values.empty())
                                idx_by_pk[tbl->row_key(rows[i])] = i;
                        }
                        for (auto& [pk, row]: tbuf.upserts) {
                            auto it = idx_by_pk.find(pk);
                            if (it != idx_by_pk.end()) {
                                rows[it->second] = row;
                            } else {
                                rows.push_back(row);
                            }
                        }
                    }
                }

                tbl->refresh_indexes();
            }

            // 写完本次提交对所有表的全部行记录后，将 commit_ts 写入全局提交日志（原子提交点）。
            // 崩溃恢复时仅回放已提交的 commit_ts，保证（含跨表）提交的原子性。
            storage_.mark_committed(commit_ts);
        } catch (...) {
            txn_manager_.mark_failed(txn_id);
            session.write_buffer.clear();
            session.read_set.clear();
        session.table_read_versions.clear();
            session.current_txn_id = 0;
            session.snapshot_ts = 0;
            row_locks_.release_all(txn_id);
            throw;
        }

        session.write_buffer.clear();
        session.read_set.clear();
        session.table_read_versions.clear();
        session.savepoints.clear();
        session.current_txn_id = 0;
        session.snapshot_ts = 0;
        // 提交成功后释放行锁，允许其他事务继续
        row_locks_.release_all(txn_id);
        bool ok_eng = storage_.commit_transaction(txn_id);
        return TxnControlResult{ ok_eng ? std::string("Transaction committed")
                                        : std::string("Transaction commit failed") };
    }

    /**
     * @brief 回滚事务：清空写缓冲与读集，释放行锁，通知事务管理器。
     * @return 回滚成功/失败的文本结果。
     */
    TxnControlResult TransactionController::rollback(Session& session) {
        if (session.current_txn_id == 0) {
            throw std::runtime_error("[Process] No active transaction to rollback");
        }
        uint64_t txn_id = session.current_txn_id;
        session.current_txn_id = 0;
        session.snapshot_ts = 0;
        session.write_buffer.clear();
        session.read_set.clear();
        session.table_read_versions.clear();
        session.savepoints.clear();
        row_locks_.release_all(txn_id);
        txn_manager_.rollback(txn_id);
        bool ok_eng = storage_.rollback_transaction(txn_id);
        return TxnControlResult{ ok_eng ? std::string("Transaction rolled back")
                                        : std::string("Transaction rollback failed") };
    }

    /**
     * @brief 在当前事务内建立命名保存点（写缓冲/读集深拷贝快照）。
     */
    TxnControlResult TransactionController::savepoint(const std::string& name, Session& session) {
        if (session.current_txn_id == 0)
            throw std::runtime_error("[Process] SAVEPOINT can only be used inside a transaction");
        session.savepoints.push_back(
                Savepoint{ name, session.write_buffer, session.read_set, session.table_read_versions });
        return TxnControlResult{ "SAVEPOINT " + name };
    }

    /**
     * @brief 回滚到保存点：恢复建立时的写缓冲/读集快照，事务继续；保存点自身保留（可重复回滚），
     * 其后建立的保存点销毁。保存点之后获得的行锁保守地保持到事务结束（不影响正确性）。
     */
    TxnControlResult TransactionController::rollback_to_savepoint(const std::string& name, Session& session) {
        if (session.current_txn_id == 0)
            throw std::runtime_error("[Process] ROLLBACK TO SAVEPOINT can only be used inside a transaction");
        for (std::size_t i = session.savepoints.size(); i > 0; --i) {
            auto& sp = session.savepoints[i - 1];
            if (sp.name != name)
                continue;
            session.write_buffer = sp.write_buffer;
            session.read_set = sp.read_set;
            session.table_read_versions = sp.table_read_versions;
            session.savepoints.resize(i); // 保留自身，销毁其后的保存点
            return TxnControlResult{ "ROLLBACK TO SAVEPOINT " + name };
        }
        throw std::runtime_error("[Process] Savepoint not found: " + name);
    }

    /**
     * @brief 销毁保存点（及其后建立的保存点），不回滚数据。
     */
    TxnControlResult TransactionController::release_savepoint(const std::string& name, Session& session) {
        if (session.current_txn_id == 0)
            throw std::runtime_error("[Process] RELEASE SAVEPOINT can only be used inside a transaction");
        for (std::size_t i = session.savepoints.size(); i > 0; --i) {
            if (session.savepoints[i - 1].name != name)
                continue;
            session.savepoints.resize(i - 1);
            return TxnControlResult{ "RELEASE SAVEPOINT " + name };
        }
        throw std::runtime_error("[Process] Savepoint not found: " + name);
    }

    /**
     * @brief 设置会话隔离级别（必须在事务外调用）。
     * @param s 包含新隔离级别代码的 SET TRANSACTION 语句。
     */
    TxnControlResult TransactionController::set_transaction(const SetTransactionStmt& s, Session& session) {
        if (session.current_txn_id != 0) {
            throw std::runtime_error("[Process] SET TRANSACTION ISOLATION LEVEL must be issued outside a transaction");
        }
        switch (s.isolation_level) {
            case 0:
                session.isolation = IsolationLevel::ReadUncommitted;
                break;
            case 1:
                session.isolation = IsolationLevel::ReadCommitted;
                break;
            case 2:
                session.isolation = IsolationLevel::RepeatableRead;
                break;
            case 3:
                session.isolation = IsolationLevel::Serializable;
                break;
            default:
                throw std::runtime_error("[Process] Unknown isolation level code");
        }
        return TxnControlResult{ "Isolation level updated" };
    }

    /**
     * @brief 为即将执行的语句分配 snapshot_ts（供读取可见性判断）和 auto_commit_ts（自动提交写操作）。
     * @param stmt 即将执行的语句，用于区分 SELECT 与 DML。
     */
    void TransactionController::prepare_for_statement(const Statement& stmt, Session& session) {
        const bool is_select = std::holds_alternative<SelectStmt>(stmt);

        if (session.in_transaction()) {
            if (session.isolation == IsolationLevel::ReadUncommitted) {
                // ReadUncommitted：读取含未提交数据的最新版本。
                session.snapshot_ts = UINT64_MAX;
            } else if (session.isolation == IsolationLevel::ReadCommitted) {
                session.snapshot_ts = txn_manager_.allocate_ts();
            }
        } else {
            // Auto-commit: always read latest committed snapshot.
            session.snapshot_ts = txn_manager_.allocate_ts();
        }

        if (!session.in_transaction() && !is_select) {
            session.auto_commit_ts = txn_manager_.allocate_ts();
        } else {
            session.auto_commit_ts = 0;
        }
    }

} // namespace corodb
