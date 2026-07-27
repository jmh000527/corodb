// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file executor.cpp
// @brief 物理算子执行器实现（PG 风格 execMain）。
//
// Executor 仅执行 DML/SELECT 物理算子；DDL（CREATE/DROP TABLE/INDEX）
// 由 commands/UtilityProcessor 处理，不经过此模块。

#include "corodb/executor/executor.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "corodb/db/session.h"
#include "corodb/executor/bool_evaluator.h"
#include "corodb/executor/expression_evaluator.h"
#include "corodb/storage/storage_engine.h"
#include "corodb/txn/row_lock_manager.h"

namespace corodb {

    // 算子内使用的求值简写：直接调用 ExpressionEvaluator/BoolEvaluator 的静态实现。
    namespace {
        inline const Value& lookup_value(const Record& r, const ColumnRef& ref) {
            return ExpressionEvaluator::lookup(r, ref);
        }
        inline const Value& lookup_value_by_name(const Record& r, const std::string& n) {
            return ExpressionEvaluator::lookup_by_name(r, n);
        }
        inline Value eval_expression(const Record& r, const Expression& e) {
            return ExpressionEvaluator::eval(r, e);
        }
        inline SqlBool evaluate_bool_expr(const BoolExpr& e, const Record& r) {
            return BoolEvaluator::eval(e, r);
        }
        inline int compare_order(const Value& a, const Value& b) {
            return BoolEvaluator::compare_order(a, b);
        }
        inline void check_timeout(const ExecutionContext& ctx) {
            if (ctx.deadline != std::chrono::steady_clock::time_point{}) {
                if (std::chrono::steady_clock::now() > ctx.deadline) {
                    throw std::runtime_error("[Executor] Query timed out");
                }
            }
        }
    } // namespace

    namespace {

        /**
         * @brief 写写冲突检测辅助：在事务路径写一行 (table, pk) 前申请行锁。
         *
         * 若 RowLockManager 缺失（早期单测）则跳过；冲突时立即抛 WriteConflictError，
         * 由 Database::execute 的 catch 块 mark_failed。
         */
        inline void txn_acquire_row_lock(const ExecutionContext& ctx, uint64_t txn_id, const std::string& table,
                                         const Value& pk) {
            auto* locks = ctx.row_locks;
            if (!locks)
                return;
            auto owner = locks->try_acquire(table, pk, txn_id);
            if (!owner.has_value())
                return;
            if (*owner == txn_id)
                return;
            throw WriteConflictError("ERROR: write-write conflict on " + table + " (locked by txn " +
                                     std::to_string(*owner) + ")");
        }

        /**
         * @brief 校验单个值是否满足列约束：NOT NULL + 基本类型族。违反抛 runtime_error。
         *
         * 类型校验采用"族"级宽松策略：数值列（Int64/Float64）接受 int64/double，
         * 文本列（Text）接受 string；NULL 仅在非 NOT NULL 列允许。
         */
        inline void validate_value(const Column& c, const Value& v) {
            if (std::holds_alternative<NullValue>(v)) {
                if (c.not_null)
                    throw std::runtime_error("ERROR: NULL value violates NOT NULL constraint on column '" + c.name +
                                             "'");
                return;
            }
            const bool is_num = std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
            const bool is_txt = std::holds_alternative<std::string>(v);
            if (c.type == TypeKind::Text && !is_txt)
                throw std::runtime_error("ERROR: type mismatch: column '" + c.name + "' expects TEXT");
            if ((c.type == TypeKind::Int64 || c.type == TypeKind::Float64) && !is_num)
                throw std::runtime_error("ERROR: type mismatch: column '" + c.name + "' expects a numeric value");
        }

        /** @brief 校验整行是否满足列约束（逐列调用 validate_value）。 */
        inline void validate_row_constraints(const std::vector<Column>& cols, const Row& row) {
            const std::size_t n = std::min(cols.size(), row.values.size());
            for (std::size_t i = 0; i < n; ++i)
                validate_value(cols[i], row.values[i]);
        }

        /** @brief 提取一行的存储主键（schema 感知，支持复合主键）；空行返回 nullopt（无主键表）。 */
        inline std::optional<Value> row_pk(const Table& t, const Row& row) {
            if (row.values.empty())
                return std::nullopt;
            return t.row_key(row);
        }

        /**
         * @brief 比较两个 Value 向量的比较器
         *
         * 用于 std::set 实现 DISTINCT 去重
         */
        struct ValueVectorCompare {
            bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
                if (a.size() != b.size())
                    return a.size() < b.size();
                for (std::size_t i = 0; i < a.size(); ++i) {
                    int cmp = compare_order(a[i], b[i]);
                    if (cmp != 0)
                        return cmp < 0;
                }
                return false;
            }
        };

        /**
         * @brief 创建全为空值的记录
         *
         * 根据给定的绑定信息创建一个所有值均为NULL的记录
         * @param bindings 绑定信息向量
         * @return Record 全为空值的记录
         */
        Record make_null_record(const std::vector<Binding>& bindings) {
            Record r;
            r.bindings = bindings;
            r.values.assign(bindings.size(), Value{ NullValue{} });
            return r;
        }

        /**
         * @brief 合并两条记录（优化版本）
         *
         * 将两条记录合并为一条，左侧记录的列在前，右侧记录的列在后。
         * 使用移动语义减少不必要的拷贝。
         *
         * @param lrec 左侧记录
         * @param rrec 右侧记录
         * @return Record 合并后的记录
         */
        Record merge_rows(const Record& lrec, const Record& rrec) {
            const std::size_t left_size = lrec.values.size();
            const std::size_t right_size = rrec.values.size();
            const std::size_t total_size = left_size + right_size;

            Record merged;
            merged.values.reserve(total_size);
            merged.bindings.reserve(total_size);

            // 使用范围insert而不是单个push_back
            merged.values.insert(merged.values.end(), lrec.values.begin(), lrec.values.end());
            merged.values.insert(merged.values.end(), rrec.values.begin(), rrec.values.end());
            merged.bindings.insert(merged.bindings.end(), lrec.bindings.begin(), lrec.bindings.end());
            merged.bindings.insert(merged.bindings.end(), rrec.bindings.begin(), rrec.bindings.end());

            return merged;
        }

        /**
         * @brief 从计划节点派生绑定信息
         *
         * 递归地从计划节点树中提取绑定信息，用于确定记录的列结构
         * @param plan 计划节点指针，指向要从中提取绑定信息的计划节点
         * @return std::vector<Binding> 派生的绑定信息向量，包含了记录中所有列的绑定信息
         */
        std::vector<Binding> derive_bindings(const PlanNode* plan) {
            // 处理顺序扫描计划节点
            if (const auto* seq = dynamic_cast<const SeqScanPlan*>(plan)) {
                std::vector<Binding> out;
                out.reserve(seq->table->columns().size());
                const std::string binding = seq->alias.empty() ? seq->table->name() : seq->alias;
                for (std::size_t i = 0; i < seq->table->columns().size(); ++i) {
                    out.push_back(Binding{ binding, seq->table->columns()[i].name, i });
                }
                return out;
            }

            // 处理过滤计划节点，直接使用子节点的绑定信息
            if (const auto* filter = dynamic_cast<const FilterPlan*>(plan)) {
                return derive_bindings(filter->child.get());
            }

            // 处理投影计划节点
            if (const auto* proj = dynamic_cast<const ProjectPlan*>(plan)) {
                std::vector<Binding> out;
                out.reserve(proj->columns.size());
                for (std::size_t i = 0; i < proj->columns.size(); ++i) {
                    const auto& item = proj->columns[i];
                    std::string name;
                    std::string table;
                    if (item.alias.has_value()) {
                        name = *item.alias;
                    } else if (std::holds_alternative<Expression>(item.value) &&
                               std::holds_alternative<ColumnRef>(std::get<Expression>(item.value))) {
                        const auto& ref = std::get<ColumnRef>(std::get<Expression>(item.value));
                        name = ref.name;
                        table = ref.table;
                    } else {
                        name = "expr" + std::to_string(i);
                    }
                    out.push_back(Binding{ table, name, out.size() });
                }
                return out;
            }

            // 处理哈希连接计划节点，合并左右子节点的绑定信息
            if (const auto* hash = dynamic_cast<const HashJoinPlan*>(plan)) {
                auto left = derive_bindings(hash->left.get());
                auto right = derive_bindings(hash->right.get());
                left.insert(left.end(), right.begin(), right.end());
                return left;
            }

            // 处理合并连接计划节点，合并左右子节点的绑定信息
            if (const auto* merge = dynamic_cast<const MergeJoinPlan*>(plan)) {
                auto left = derive_bindings(merge->left.get());
                auto right = derive_bindings(merge->right.get());
                left.insert(left.end(), right.begin(), right.end());
                return left;
            }

            // 处理嵌套循环连接计划节点，合并左右子节点的绑定信息
            if (const auto* nl = dynamic_cast<const NestedLoopJoinPlan*>(plan)) {
                auto left = derive_bindings(nl->left.get());
                auto right = derive_bindings(nl->right.get());
                left.insert(left.end(), right.begin(), right.end());
                return left;
            }

            // 处理聚合计划节点
            if (const auto* agg = dynamic_cast<const AggregatePlan*>(plan)) {
                std::vector<Binding> out;
                out.reserve(agg->projections.size());
                for (std::size_t i = 0; i < agg->projections.size(); ++i) {
                    const auto& item = agg->projections[i];
                    std::string name;
                    std::string table;
                    if (item.alias.has_value()) {
                        name = *item.alias;
                    } else if (std::holds_alternative<AggregateExpr>(item.value)) {
                        const auto& a = std::get<AggregateExpr>(item.value);
                        name = a.arg ? a.arg->name : "count";
                        table = a.arg ? a.arg->table : "";
                    } else {
                        const auto& expr = std::get<Expression>(item.value);
                        if (std::holds_alternative<ColumnRef>(expr)) {
                            const auto& ref = std::get<ColumnRef>(expr);
                            name = ref.name;
                            table = ref.table;
                        } else {
                            name = "expr" + std::to_string(i);
                        }
                    }
                    out.push_back(Binding{ table, name, out.size() });
                }
                return out;
            }

            // 处理排序计划节点，直接使用子节点的绑定信息
            if (const auto* ord = dynamic_cast<const OrderByPlan*>(plan)) {
                return derive_bindings(ord->child.get());
            }

            // 处理限制计划节点，直接使用子节点的绑定信息
            if (const auto* lim = dynamic_cast<const LimitPlan*>(plan)) {
                return derive_bindings(lim->child.get());
            }

            // 默认返回空绑定信息
            return {};
        }

    } // namespace

    // [moved] lookup_value/eval_scalar_function/eval_expression/compare_order/
    // evaluate_comparison/evaluate_bool_expr -> 见 expression_evaluator.cpp / bool_evaluator.cpp.

    /**
     * @brief 从行数据创建记录
     *
     * 将表的行数据转换为记录格式，添加绑定信息
     * @param table 表对象
     * @param row 行数据
     * @param binding_name 绑定名称
     * @return Record 转换后的记录
     */
    Record make_record_from_row(const Table& table, const Row& row, const std::string& binding_name) {
        Record rec;
        rec.values = row.values;
        rec.bindings.reserve(table.columns().size());
        for (std::size_t i = 0; i < table.columns().size(); ++i) {
            rec.bindings.push_back(Binding{ binding_name, table.columns()[i].name, i });
        }
        return rec;
    }

    /**
     * @brief 执行顺序扫描计划
     *
     * 遍历表中的所有行，生成对应的记录
     * @param plan 顺序扫描计划
     * @return std::generator<Record> 记录生成器
     */
    std::generator<Record> execute_seq_scan(const ExecutionContext& ctx, const SeqScanPlan& plan) {
        const std::string binding = plan.alias.empty() ? plan.table->name() : plan.alias;
        // 事务隔离：若当前 Session 在事务中，需要叠加 TxnWriteBuffer：
        //   1) 跳过被 deletes 标记的 pk
        //   2) 命中 upserts 的 pk 用 buffered 行（read-your-own-writes）
        //   3) 在表扫描完成后，把 upserts 中 pk 不在 rows_ 的纯 INSERT 输出
        const TableTxnBuffer* buf = nullptr;
        Session* sess = ctx.session.get();
        if (sess && sess->in_transaction()) {
            buf = sess->write_buffer.find_table(plan.table->name());
        }

        // snapshot_ts 生效时走快照读路径，既覆盖事务内一致性，也覆盖自动提交查询。
        std::unordered_set<Value, ValueHash, ValueEq> emitted_buffer_keys;
        const uint64_t snapshot_ts = sess ? sess->snapshot_ts : 0;
        // 只有 SERIALIZABLE 隔离才记录读集，提交阶段再做验证。
        const bool track_read_set = sess && sess->in_transaction() && sess->isolation == IsolationLevel::Serializable;
        // Serializable 幻读防护：读取时记录表的 write_version。
        if (track_read_set) {
            sess->table_read_versions[plan.table->name()] = plan.table->write_counter();
        }
        auto record_read_pk = [&](const Row& row) {
            if (!track_read_set)
                return;
            if (row.values.empty())
                return;
            sess->read_set[plan.table->name()].insert(plan.table->row_key(row));
        };
        // 流式扫描：存储型表逐行归并获取（不物化全量结果）；无 snapshot 时取最新已提交（MAX）；
        // 无存储的纯内存表（测试夹具）则 yield rows_。
        const uint64_t snap = snapshot_ts != 0 ? snapshot_ts : std::numeric_limits<uint64_t>::max();
        check_timeout(ctx);
        for (auto&& row: plan.table->scan_visible_stream(snap)) {
            if (buf && !row.values.empty()) {
                const Value pk = plan.table->row_key(row);
                if (buf->deletes.count(pk))
                    continue;
                auto it = buf->upserts.find(pk);
                if (it != buf->upserts.end()) {
                    record_read_pk(row);
                    co_yield make_record_from_row(*plan.table, it->second, binding);
                    emitted_buffer_keys.insert(pk);
                    continue;
                }
            }
            record_read_pk(row);
            check_timeout(ctx);
            co_yield make_record_from_row(*plan.table, row, binding);
        }
        if (buf) {
            for (const auto& [pk, row]: buf->upserts) {
                if (emitted_buffer_keys.count(pk))
                    continue;
                co_yield make_record_from_row(*plan.table, row, binding);
            }
        }
    }

    /**
     * @brief 执行查询计划的入口函数
     *
     * 根据计划节点类型调用相应的执行函数，使用协程生成记录流
     * @param plan 计划节点指针
     * @return std::generator<Record> 记录生成器
     * @throws std::runtime_error 如果计划节点类型不支持
     */
    std::generator<Record> execute_plan(const ExecutionContext& ctx, const PlanNode* plan) {
        // 检查计划节点是否为空
        if (!plan) {
            throw std::runtime_error("[Executor] Execution plan node is null");
        }

        // 处理顺序扫描计划节点
        if (const auto* seq = dynamic_cast<const SeqScanPlan*>(plan)) {
            for (auto&& rec: execute_seq_scan(ctx, *seq)) {
                co_yield rec;
            }
            co_return;
        }

        // 处理索引扫描计划节点
        if (const auto* idx = dynamic_cast<const IndexScanPlan*>(plan)) {
            const std::string binding = idx->alias.empty() ? idx->table->name() : idx->alias;
            Session* sess = ctx.session.get();
            const uint64_t snapshot_ts = sess ? sess->snapshot_ts : 0;
            // 事务私有 buffer 覆盖（read-your-own-writes）
            const TableTxnBuffer* buf = nullptr;
            if (sess && sess->in_transaction()) {
                buf = sess->write_buffer.find_table(idx->table->name());
            }
            const bool track_read_set =
                    sess && sess->in_transaction() && sess->isolation == IsolationLevel::Serializable;
            if (track_read_set) {
                sess->table_read_versions[idx->table->name()] = idx->table->write_counter();
            }
            auto record_idx_read = [&](const Row& row) {
                if (!track_read_set)
                    return;
                if (row.values.empty())
                    return;
                sess->read_set[idx->table->name()].insert(idx->table->row_key(row));
            };

            // 索引列在表中的下标（用于可见性重查过滤陈旧超集条目）。
            const std::size_t col_idx = idx->table->find_column(idx->column).value_or(0);
            // 复合索引各列下标（复合等值重查用）。
            std::vector<std::size_t> comp_idxs;
            if (idx->is_composite) {
                comp_idxs.reserve(idx->composite_columns.size());
                for (const auto& cn: idx->composite_columns)
                    comp_idxs.push_back(idx->table->find_column(cn).value_or(0));
            }
            // 无 snapshot（sessionless / 非 MVCC）时取最新已提交版本。
            const uint64_t snap = (snapshot_ts != 0) ? snapshot_ts : std::numeric_limits<uint64_t>::max();

            // 索引键匹配判定：等值用 ValueEq；范围用 low/high + inclusive。
            auto matches = [&](const Value& v) -> bool {
                if (idx->is_in) {
                    for (const auto& k: idx->in_keys)
                        if (ValueEq{}(v, k))
                            return true;
                    return false;
                }
                if (!idx->is_range)
                    return ValueEq{}(v, idx->key);
                if (idx->low.has_value()) {
                    if (idx->low_inclusive) {
                        if (ValueLess{}(v, *idx->low))
                            return false; // v < low
                    } else if (!ValueLess{}(*idx->low, v)) {
                        return false; // !(low < v)
                    }
                }
                if (idx->high.has_value()) {
                    if (idx->high_inclusive) {
                        if (ValueLess{}(*idx->high, v))
                            return false; // high < v
                    } else if (!ValueLess{}(v, *idx->high)) {
                        return false; // !(v < high)
                    }
                }
                return true;
            };
            // 复合等值：所有复合列都需与键相等（超集重查）；否则回退到单列值匹配。
            auto row_matches = [&](const Row& row) -> bool {
                if (idx->is_composite) {
                    for (std::size_t i = 0; i < comp_idxs.size(); ++i) {
                        if (comp_idxs[i] >= row.values.size())
                            return false;
                        if (!ValueEq{}(row.values[comp_idxs[i]], idx->composite_key[i]))
                            return false;
                    }
                    return true;
                }
                return col_idx < row.values.size() && matches(row.values[col_idx]);
            };
            // 通过索引取候选主键；对每个 pk 做可见性重查 + 键匹配重查。
            std::vector<Value> pks;
            if (idx->is_composite) {
                pks = idx->table->lookup_index_composite(idx->index_name, idx->composite_key);
            } else if (idx->is_in) {
                // IN：多个等值点查的并集（去重）。
                std::unordered_set<Value, ValueHash, ValueEq> seen_pk;
                for (const auto& k: idx->in_keys) {
                    for (const auto& p: idx->table->lookup_index(idx->column, k)) {
                        if (seen_pk.insert(p).second)
                            pks.push_back(p);
                    }
                }
            } else if (idx->is_range) {
                pks = idx->table->lookup_index_range(idx->column, idx->low, idx->low_inclusive, idx->high,
                                                     idx->high_inclusive);
            } else {
                pks = idx->table->lookup_index(idx->column, idx->key);
            }
            std::unordered_set<Value, ValueHash, ValueEq> emitted;
            check_timeout(ctx);
            for (const Value& pk: pks) {
                if (buf) {
                    if (buf->deletes.count(pk)) {
                        emitted.insert(pk);
                        continue;
                    }
                    auto bit = buf->upserts.find(pk);
                    if (bit != buf->upserts.end()) {
                        const Row& br = bit->second; // 事务缓冲覆盖：用缓冲行，且需仍匹配索引键
                        if (row_matches(br)) {
                            record_idx_read(br);
                            co_yield make_record_from_row(*idx->table, br, binding);
                        }
                        emitted.insert(pk);
                        continue;
                    }
                }
                auto row = idx->table->lookup_visible(pk, snap);
                if (!row.has_value())
                    continue; // 已删除 / 不可见
                // 可见性重查：超集索引可能含陈旧值，需确认可见版本的索引列仍满足键条件。
                if (!row_matches(*row))
                    continue;
                emitted.insert(pk);
                record_idx_read(*row);
                co_yield make_record_from_row(*idx->table, *row, binding);
            }
            // 事务缓冲中匹配键但不在索引/未发出的纯新增行。
            if (buf) {
                for (const auto& [pk, row]: buf->upserts) {
                    if (emitted.count(pk))
                        continue;
                    if (row_matches(row))
                        co_yield make_record_from_row(*idx->table, row, binding);
                }
            }
            co_return;
        }

        // 处理过滤计划节点
        if (const auto* filter = dynamic_cast<const FilterPlan*>(plan)) {
            auto child_gen = execute_plan(ctx, filter->child.get());
            for (auto&& rec: child_gen) {
                // 只返回满足过滤条件的记录
                if (evaluate_bool_expr(filter->predicate, rec) == SqlBool::True) {
                    co_yield rec;
                }
            }
            co_return;
        }

        // 处理投影计划节点
        if (const auto* proj = dynamic_cast<const ProjectPlan*>(plan)) {
            auto child_gen = execute_plan(ctx, proj->child.get());

            // DISTINCT 去重使用 set 存储已见过的值组合
            std::set<std::vector<Value>, ValueVectorCompare> seen;

            for (auto&& rec: child_gen) {
                Record out;
                out.values.reserve(proj->columns.size());
                out.bindings.reserve(proj->columns.size());

                // 计算每个投影列的值
                for (std::size_t i = 0; i < proj->columns.size(); ++i) {
                    const auto& item = proj->columns[i];
                    if (std::holds_alternative<AggregateExpr>(item.value)) {
                        throw std::runtime_error("[Executor] Aggregates should be planned via AggregatePlan");
                    }
                    const auto& expr = std::get<Expression>(item.value);
                    Value v = eval_expression(rec, expr);
                    out.values.push_back(v);

                    // 确定列名和表名
                    std::string name;
                    std::string table;
                    if (item.alias.has_value()) {
                        name = *item.alias;
                    } else if (std::holds_alternative<ColumnRef>(expr)) {
                        const auto& ref = std::get<ColumnRef>(expr);
                        name = ref.name;
                        table = ref.table;
                    } else {
                        name = "expr" + std::to_string(i);
                    }
                    out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                }

                // 如果启用了 DISTINCT，检查是否为重复行
                if (proj->distinct) {
                    auto [it, inserted] = seen.insert(out.values);
                    if (!inserted) {
                        continue; // 跳过重复行
                    }
                }

                co_yield out;
            }
            co_return;
        }

        // DDL 计划节点（CreateTable/CreateIndex/DropTable/DropIndex）由
        // commands/UtilityProcessor 处理，不应到达 Executor。

        // 处理排序计划节点
        if (const auto* order = dynamic_cast<const OrderByPlan*>(plan)) {
            // 计算排序键
            auto eval_key = [&](const Record& r, const std::variant<Expression, AggregateExpr>& key) -> Value {
                if (std::holds_alternative<Expression>(key)) {
                    return eval_expression(r, std::get<Expression>(key));
                }
                const auto& agg = std::get<AggregateExpr>(key);
                std::string name = agg.arg ? agg.arg->name : "count";
                return lookup_value_by_name(r, name);
            };

            // 排序比较器（用于正常排序，a < b）
            auto comparator = [&](const Record& a, const Record& b) {
                for (const auto& item: order->items) {
                    const auto& va = eval_key(a, item.key);
                    const auto& vb = eval_key(b, item.key);
                    int ord = compare_order(va, vb);
                    if (ord == 0)
                        continue;
                    return item.asc ? ord < 0 : ord > 0;
                }
                return false;
            };

            // 计算 Top-N 的 K 值
            std::optional<std::size_t> topn_k;
            if (order->limit.has_value()) {
                int64_t k = order->limit.value();
                if (order->offset.has_value()) {
                    k += order->offset.value();
                }
                if (k > 0) {
                    topn_k = static_cast<std::size_t>(k);
                }
            }

            if (topn_k.has_value() && topn_k.value() > 0) {
                // 【Top-N 优化】：使用最大堆只保留前 K 个最小元素
                // 堆顶是"最大"的元素，当新元素比堆顶小时替换
                std::size_t k = topn_k.value();

                // 反向比较器（用于最大堆，a > b）
                auto heap_comparator = [&](const Record& a, const Record& b) {
                    return comparator(a, b); // 最小堆：a < b 时 a 优先级更高
                };

                std::vector<Record> heap;
                heap.reserve(k + 1);

                for (auto&& rec: execute_plan(ctx, order->child.get())) {
                    if (heap.size() < k) {
                        // 堆未满，直接添加
                        heap.push_back(std::move(rec));
                        std::push_heap(heap.begin(), heap.end(), heap_comparator);
                    } else {
                        // 堆已满，检查新元素是否应该替换堆顶
                        // 如果新元素 < 堆顶，则替换
                        if (comparator(rec, heap.front())) {
                            std::pop_heap(heap.begin(), heap.end(), heap_comparator);
                            heap.back() = std::move(rec);
                            std::push_heap(heap.begin(), heap.end(), heap_comparator);
                        }
                    }
                }

                // 从堆中按顺序提取结果
                std::sort_heap(heap.begin(), heap.end(), heap_comparator);

                for (auto& rec: heap)
                    co_yield rec;
            } else {
                // 【常规排序】：物化所有结果后排序
                std::vector<Record> materialized;
                for (auto&& rec: execute_plan(ctx, order->child.get())) {
                    materialized.push_back(std::move(rec));
                }

                // 对结果进行排序
                std::stable_sort(materialized.begin(), materialized.end(), comparator);

                // 生成排序后的记录
                for (auto& rec: materialized)
                    co_yield rec;
            }
            co_return;
        }

        // 处理哈希连接计划节点
        if (const auto* hash = dynamic_cast<const HashJoinPlan*>(plan)) {
            auto left_schema = derive_bindings(hash->left.get());
            auto right_schema = derive_bindings(hash->right.get());
            Record null_left = make_null_record(left_schema);
            Record null_right = make_null_record(right_schema);

            // 桶行结构，用于记录匹配状态
            struct BucketRow {
                Record rec;
                bool matched{ false };
            };

            std::vector<BucketRow> right_rows;
            std::unordered_map<Value, std::vector<std::size_t>, ValueHash, ValueEq> buckets;

            // 先收集所有右侧行以确定大小
            for (auto&& r: execute_plan(ctx, hash->right.get())) {
                right_rows.push_back(BucketRow{ std::move(r), false });
            }

            // 预分配哈希表容量，减少重哈希开销
            // 假设键的基数大约为行数的一半
            buckets.reserve(right_rows.size() / 2 + 1);

            // 构建哈希表
            for (std::size_t i = 0; i < right_rows.size(); ++i) {
                Value key = lookup_value(right_rows[i].rec, hash->right_key);
                buckets[key].push_back(i);
            }

            // 执行连接
            for (auto&& l: execute_plan(ctx, hash->left.get())) {
                check_timeout(ctx);
                bool matched = false;
                Value key = lookup_value(l, hash->left_key);
                auto it = buckets.find(key);
                if (it != buckets.end()) {
                    for (auto idx: it->second) {
                        auto& br = right_rows[idx];
                        Record merged = merge_rows(l, br.rec);
                        // 检查剩余条件
                        if (hash->residual.has_value()) {
                            if (evaluate_bool_expr(*hash->residual, merged) != SqlBool::True)
                                continue;
                        }
                        matched = true;
                        br.matched = true;
                        co_yield merged;
                    }
                }
                // 处理左连接和全连接的不匹配情况
                if (!matched && (hash->type == JoinType::Left || hash->type == JoinType::Full)) {
                    co_yield merge_rows(l, null_right);
                }
            }

            // 处理右连接和全连接的不匹配情况
            if (hash->type == JoinType::Right || hash->type == JoinType::Full) {
                for (const auto& br: right_rows) {
                    if (br.matched)
                        continue;
                    co_yield merge_rows(null_left, br.rec);
                }
            }
            co_return;
        }

        // 处理合并连接计划节点
        if (const auto* merge = dynamic_cast<const MergeJoinPlan*>(plan)) {
            // 物化左右两边的结果，同时提取并缓存键值
            struct KeyedRecord {
                Record rec;
                Value key;
            };

            std::vector<KeyedRecord> left_rows;
            for (auto&& l: execute_plan(ctx, merge->left.get())) {
                Value k = lookup_value(l, merge->left_key);
                left_rows.push_back(KeyedRecord{ std::move(l), std::move(k) });
            }

            std::vector<KeyedRecord> right_rows;
            for (auto&& r: execute_plan(ctx, merge->right.get())) {
                Value k = lookup_value(r, merge->right_key);
                right_rows.push_back(KeyedRecord{ std::move(r), std::move(k) });
            }

            // 若子计划未排序，则按连接键对左右两侧排序。
            auto less_l = [](const KeyedRecord& a, const KeyedRecord& b) { return compare_order(a.key, b.key) < 0; };
            auto less_r = [](const KeyedRecord& a, const KeyedRecord& b) { return compare_order(a.key, b.key) < 0; };

            if (!merge->left_sorted) {
                std::stable_sort(left_rows.begin(), left_rows.end(), less_l);
            }
            if (!merge->right_sorted) {
                std::stable_sort(right_rows.begin(), right_rows.end(), less_r);
            }

            // 创建NULL记录
            auto left_schema = derive_bindings(merge->left.get());
            auto right_schema = derive_bindings(merge->right.get());
            Record null_left = make_null_record(left_schema);
            Record null_right = make_null_record(right_schema);

            // 记录匹配状态
            std::vector<bool> left_matched(left_rows.size(), false);
            std::vector<bool> right_matched(right_rows.size(), false);

            // 执行合并连接（使用缓存的键值）
            std::size_t i = 0, j = 0;
            while (i < left_rows.size() && j < right_rows.size()) {
                check_timeout(ctx);
                const Value& lv = left_rows[i].key;
                const Value& rv = right_rows[j].key;
                int ord = compare_order(lv, rv);

                // 左表当前行小于右表当前行
                if (ord < 0) {
                    if (merge->type == JoinType::Left || merge->type == JoinType::Full) {
                        co_yield merge_rows(left_rows[i].rec, null_right);
                    }
                    ++i;
                    continue;
                }

                // 右表当前行小于左表当前行
                if (ord > 0) {
                    if (merge->type == JoinType::Right || merge->type == JoinType::Full) {
                        co_yield merge_rows(null_left, right_rows[j].rec);
                    }
                    ++j;
                    continue;
                }

                // 找到匹配的键范围（使用缓存的键值）
                std::size_t i_start = i;
                std::size_t j_start = j;
                while (i < left_rows.size() && compare_order(left_rows[i].key, lv) == 0)
                    ++i;
                while (j < right_rows.size() && compare_order(right_rows[j].key, rv) == 0)
                    ++j;

                // 生成所有匹配组合
                for (std::size_t li = i_start; li < i; ++li) {
                    for (std::size_t rj = j_start; rj < j; ++rj) {
                        Record merged = merge_rows(left_rows[li].rec, right_rows[rj].rec);
                        // 检查剩余条件
                        if (merge->residual.has_value()) {
                            if (evaluate_bool_expr(*merge->residual, merged) != SqlBool::True)
                                continue;
                        }
                        left_matched[li] = true;
                        right_matched[rj] = true;
                        co_yield merged;
                    }
                }
            }

            // 处理左表剩余行
            if (merge->type == JoinType::Left || merge->type == JoinType::Full) {
                for (; i < left_rows.size(); ++i) {
                    if (!left_matched[i]) {
                        co_yield merge_rows(left_rows[i].rec, null_right);
                    }
                }
            }

            // 处理右表剩余行
            if (merge->type == JoinType::Right || merge->type == JoinType::Full) {
                for (; j < right_rows.size(); ++j) {
                    if (!right_matched[j]) {
                        co_yield merge_rows(null_left, right_rows[j].rec);
                    }
                }
            }
            co_return;
        }

        // 处理限制计划节点
        if (const auto* lim = dynamic_cast<const LimitPlan*>(plan)) {
            auto child = execute_plan(ctx, lim->child.get());
            int64_t skipped = 0;
            int64_t emitted = 0;
            const int64_t off = lim->offset.value_or(0);
            const int64_t limv = lim->limit.value_or(std::numeric_limits<int64_t>::max());

            // 跳过偏移量
            for (auto&& rec: child) {
                if (skipped < off) {
                    ++skipped;
                    continue;
                }
                // 限制输出数量
                if (emitted >= limv)
                    break;
                ++emitted;
                co_yield rec;
            }
            co_return;
        }

        // 处理嵌套循环连接计划节点
        if (const auto* join = dynamic_cast<const NestedLoopJoinPlan*>(plan)) {
            // 物化左右两边的结果
            auto left_gen = execute_plan(ctx, join->left.get());
            std::vector<Record> left_rows;
            for (auto&& l: left_gen)
                left_rows.push_back(std::move(l));

            auto right_gen = execute_plan(ctx, join->right.get());
            std::vector<Record> right_rows;
            for (auto&& r: right_gen)
                right_rows.push_back(std::move(r));

            // 创建NULL记录
            auto left_schema = derive_bindings(join->left.get());
            auto right_schema = derive_bindings(join->right.get());
            Record null_left = make_null_record(left_schema);
            Record null_right = make_null_record(right_schema);

            // 记录匹配状态
            std::vector<bool> right_matched(right_rows.size(), false);

            // 执行嵌套循环连接
            for (const auto& l: left_rows) {
                bool matched = false;
                for (std::size_t idx = 0; idx < right_rows.size(); ++idx) {
                    const auto& r = right_rows[idx];
                    Record merged = merge_rows(l, r);
                    if (evaluate_bool_expr(join->on, merged) == SqlBool::True) {
                        matched = true;
                        right_matched[idx] = true;
                        co_yield merged;
                    }
                }
                // 处理左连接和全连接的不匹配情况
                if (!matched && (join->type == JoinType::Left || join->type == JoinType::Full)) {
                    co_yield merge_rows(l, null_right);
                }
            }

            // 处理右连接和全连接的不匹配情况
            if (join->type == JoinType::Right || join->type == JoinType::Full) {
                for (std::size_t idx = 0; idx < right_rows.size(); ++idx) {
                    if (right_matched[idx])
                        continue;
                    co_yield merge_rows(null_left, right_rows[idx]);
                }
            }
            co_return;
        }

        // 处理聚合计划节点
        if (const auto* agg = dynamic_cast<const AggregatePlan*>(plan)) {
            // 聚合状态结构
            struct AggState {
                int64_t count{ 0 };
                int64_t sum{ 0 };
                std::optional<Value> min;
                std::optional<Value> max;
            };

            // 聚合结果最终化函数
            auto finalize = [](const AggState& st, AggFunc func) -> Value {
                switch (func) {
                    case AggFunc::Count:
                        return Value{ st.count };
                    case AggFunc::Sum:
                        return st.count == 0 ? Value{ NullValue{} } : Value{ st.sum };
                    case AggFunc::Avg:
                        if (st.count == 0)
                            return Value{ NullValue{} };
                        return Value{ static_cast<double>(st.sum) / static_cast<double>(st.count) };
                    case AggFunc::Min:
                        return st.min.has_value() ? *st.min : Value{ NullValue{} };
                    case AggFunc::Max:
                        return st.max.has_value() ? *st.max : Value{ NullValue{} };
                }
                return Value{ NullValue{} };
            };

            // 分组数据结构
            struct GroupData {
                std::vector<Value> key;
                std::vector<AggState> states;
            };

            // 分组键类型和哈希函数
            using Key = std::vector<Value>;
            struct KeyHash {
                std::size_t operator()(const Key& k) const {
                    std::size_t h = 0;
                    for (const auto& v: k) {
                        h ^= ValueHash{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    }
                    return h;
                }
            };
            struct KeyEq {
                bool operator()(const Key& a, const Key& b) const {
                    if (a.size() != b.size())
                        return false;
                    for (std::size_t i = 0; i < a.size(); ++i) {
                        if (!ValueEq{}(a[i], b[i]))
                            return false;
                    }
                    return true;
                }
            };

            // 查找聚合函数在聚合列表中的索引 - O(n)复杂度，但更可靠
            auto agg_index = [&](const AggregateExpr& needle) -> std::size_t {
                for (std::size_t i = 0; i < agg->aggregates.size(); ++i) {
                    const auto& a = agg->aggregates[i];
                    if (a.func != needle.func)
                        continue;
                    const bool lhs_has = a.arg.has_value();
                    const bool rhs_has = needle.arg.has_value();
                    if (lhs_has != rhs_has)
                        continue;
                    if (!lhs_has || (a.arg->table == needle.arg->table && a.arg->name == needle.arg->name))
                        return i;
                }
                throw std::runtime_error("[Executor] Aggregate function not found in plan");
            };

            // 处理全局聚合（无分组），避免使用哈希表，提高性能
            if (agg->group_by.empty()) {
                GroupData global;
                global.states.resize(agg->aggregates.size());

                // 快速路径：全部聚合都是 COUNT(*)（无参数）且 child 是 SeqScanPlan，
                // 没有事务 buffer 也无须记录读集时，直接用 scan_visible 的大小完成计数，
                // 跳过 record 物化的开销。
                bool all_count_star = !agg->aggregates.empty() && !agg->having.has_value();
                for (const auto& a: agg->aggregates) {
                    if (a.func != AggFunc::Count || a.arg.has_value()) {
                        all_count_star = false;
                        break;
                    }
                }
                const auto* seq_child = dynamic_cast<const SeqScanPlan*>(agg->child.get());
                if (all_count_star && seq_child) {
                    Session* sess_a = ctx.session.get();
                    const TableTxnBuffer* buf_a = nullptr;
                    if (sess_a && sess_a->in_transaction()) {
                        buf_a = sess_a->write_buffer.find_table(seq_child->table->name());
                    }
                    const bool track_read_set_a =
                            sess_a && sess_a->in_transaction() && sess_a->isolation == IsolationLevel::Serializable;
                    if (!track_read_set_a && (!buf_a || (buf_a->upserts.empty() && buf_a->deletes.empty()))) {
                        const uint64_t snapshot_ts_a = sess_a ? sess_a->snapshot_ts : 0;
                        std::size_t row_count =
                                seq_child->table->has_storage()
                                        ? seq_child->table
                                                  ->scan_visible(snapshot_ts_a != 0
                                                                         ? snapshot_ts_a
                                                                         : std::numeric_limits<uint64_t>::max())
                                                  .size()
                                        : seq_child->table->rows().size();
                        for (auto& st: global.states) {
                            st.count = static_cast<int64_t>(row_count);
                        }
                        Record out;
                        out.values.reserve(agg->projections.size());
                        out.bindings.reserve(agg->projections.size());
                        for (std::size_t i = 0; i < agg->projections.size(); ++i) {
                            const auto& item = agg->projections[i];
                            if (std::holds_alternative<AggregateExpr>(item.value)) {
                                const auto& a = std::get<AggregateExpr>(item.value);
                                auto pos = agg_index(a);
                                out.values.push_back(finalize(global.states[pos], a.func));
                                std::string name =
                                        item.alias.has_value() ? *item.alias : (a.arg ? a.arg->name : "count");
                                std::string table = a.arg ? a.arg->table : "";
                                out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                            } else {
                                throw std::runtime_error("[Executor] Non-aggregate expression in global aggregation");
                            }
                        }
                        co_yield out;
                        co_return;
                    }
                }

                auto child = execute_plan(ctx, agg->child.get());
                // 直接更新全局聚合状态
                for (auto&& rec: child) {
                    // 更新每个聚合函数的状态
                    for (std::size_t i = 0; i < agg->aggregates.size(); ++i) {
                        auto& a = agg->aggregates[i];
                        auto& st = global.states[i];
                        switch (a.func) {
                            case AggFunc::Count:
                                if (!a.arg.has_value()) {
                                    ++st.count;
                                } else {
                                    const auto& v = lookup_value(rec, *a.arg);
                                    if (!std::holds_alternative<NullValue>(v))
                                        ++st.count;
                                }
                                break;
                            case AggFunc::Sum:
                            case AggFunc::Avg: {
                                if (!a.arg.has_value())
                                    throw std::runtime_error("[Executor] SUM/AVG requires a column argument");
                                const auto& v = lookup_value(rec, *a.arg);
                                if (std::holds_alternative<NullValue>(v))
                                    break;
                                if (std::holds_alternative<double>(v))
                                    st.sum += static_cast<int64_t>(std::get<double>(v));
                                else if (std::holds_alternative<int64_t>(v))
                                    st.sum += std::get<int64_t>(v);
                                else
                                    throw std::runtime_error("[Executor] SUM/AVG requires numeric values");
                                ++st.count;
                                break;
                            }
                            case AggFunc::Min: {
                                if (!a.arg.has_value())
                                    throw std::runtime_error("[Executor] MIN requires a column argument");
                                const auto& v = lookup_value(rec, *a.arg);
                                if (std::holds_alternative<NullValue>(v))
                                    break;
                                if (!st.min.has_value() || compare_order(v, *st.min) < 0)
                                    st.min = v;
                                break;
                            }
                            case AggFunc::Max: {
                                if (!a.arg.has_value())
                                    throw std::runtime_error("[Executor] MAX requires a column argument");
                                const auto& v = lookup_value(rec, *a.arg);
                                if (std::holds_alternative<NullValue>(v))
                                    break;
                                if (!st.max.has_value() || compare_order(v, *st.max) > 0)
                                    st.max = v;
                                break;
                            }
                        }
                    }
                }

                Record out;
                out.values.reserve(agg->projections.size());
                out.bindings.reserve(agg->projections.size());

                // 生成输出记录
                for (std::size_t i = 0; i < agg->projections.size(); ++i) {
                    const auto& item = agg->projections[i];
                    if (std::holds_alternative<AggregateExpr>(item.value)) {
                        const auto& a = std::get<AggregateExpr>(item.value);
                        auto pos = agg_index(a);
                        out.values.push_back(finalize(global.states[pos], a.func));
                        std::string name = item.alias.has_value() ? *item.alias : (a.arg ? a.arg->name : "count");
                        std::string table = a.arg ? a.arg->table : "";
                        out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                    } else {
                        throw std::runtime_error("[Executor] Non-aggregate expression in global aggregation");
                    }
                }

                // 若存在 HAVING，追加非投影聚合到 Record 以便求值。
                if (agg->having.has_value()) {
                    for (std::size_t ai = 0; ai < agg->aggregates.size(); ++ai) {
                        const auto& a = agg->aggregates[ai];
                        bool in_proj = false;
                        for (const auto& item : agg->projections) {
                            if (auto* pa = std::get_if<AggregateExpr>(&item.value)) {
                                if (pa->func == a.func &&
                                    ((pa->arg.has_value() && a.arg.has_value() && pa->arg->name == a.arg->name) ||
                                     (!pa->arg.has_value() && !a.arg.has_value()))) {
                                    in_proj = true; break;
                                }
                            }
                        }
                        if (!in_proj) {
                            out.values.push_back(finalize(global.states[ai], a.func));
                            std::string name = a.arg ? a.arg->name : "count";
                            std::string table = a.arg ? a.arg->table : "";
                            out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                        }
                    }
                    if (evaluate_bool_expr(*agg->having, out) != SqlBool::True) {
                        co_return;
                    }
                }
                co_yield out;
                co_return;
            }

            // 共享的"按一行更新一组聚合状态"逻辑（Hash / Sort 两条路径共用）。
            auto update_states = [&](std::vector<AggState>& states, const Record& rec) {
                for (std::size_t i = 0; i < agg->aggregates.size(); ++i) {
                    auto& a = agg->aggregates[i];
                    auto& st = states[i];
                    switch (a.func) {
                        case AggFunc::Count:
                            if (!a.arg.has_value()) {
                                ++st.count;
                            } else {
                                const auto& v = lookup_value(rec, *a.arg);
                                if (!std::holds_alternative<NullValue>(v))
                                    ++st.count;
                            }
                            break;
                        case AggFunc::Sum:
                        case AggFunc::Avg: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] SUM/AVG requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!std::holds_alternative<int64_t>(v))
                                throw std::runtime_error("[Executor] SUM/AVG only supports int64 values");
                            st.sum += std::get<int64_t>(v);
                            ++st.count;
                            break;
                        }
                        case AggFunc::Min: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] MIN requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!st.min.has_value() || compare_order(v, *st.min) < 0)
                                st.min = v;
                            break;
                        }
                        case AggFunc::Max: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] MAX requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!st.max.has_value() || compare_order(v, *st.max) > 0)
                                st.max = v;
                            break;
                        }
                    }
                }
            };

            // 共享的"组 → 输出 Record"emit 逻辑（应用 projections + HAVING）。
            auto emit_group = [&](const Key& key, const std::vector<AggState>& states) -> std::optional<Record> {
                Record out;
                out.values.reserve(agg->projections.size());
                out.bindings.reserve(agg->projections.size());
                auto find_group_index = [&](const ColumnRef& col) -> std::size_t {
                    for (std::size_t i = 0; i < agg->group_by.size(); ++i) {
                        const auto& g = agg->group_by[i];
                        if ((g.table.empty() || g.table == col.table) && g.name == col.name)
                            return i;
                    }
                    throw std::runtime_error("[Executor] Column not part of GROUP BY: " + col.name);
                };
                for (std::size_t i = 0; i < agg->projections.size(); ++i) {
                    const auto& item = agg->projections[i];
                    if (std::holds_alternative<AggregateExpr>(item.value)) {
                        const auto& a = std::get<AggregateExpr>(item.value);
                        auto pos = agg_index(a);
                        out.values.push_back(finalize(states[pos], a.func));
                        std::string name = item.alias.has_value() ? *item.alias : (a.arg ? a.arg->name : "count");
                        std::string table = a.arg ? a.arg->table : "";
                        out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                    } else {
                        const auto& expr = std::get<Expression>(item.value);
                        if (!std::holds_alternative<ColumnRef>(expr))
                            throw std::runtime_error(
                                    "[Executor] Only GROUP BY columns allowed as non-aggregate projections");
                        const auto& col = std::get<ColumnRef>(expr);
                        auto idx = find_group_index(col);
                        out.values.push_back(key[idx]);
                        std::string name = item.alias.has_value() ? *item.alias : col.name;
                        out.bindings.push_back(Binding{ col.table, name, out.bindings.size() });
                    }
                }
                if (agg->having.has_value() && evaluate_bool_expr(*agg->having, out) != SqlBool::True)
                    return std::nullopt;
                return out;
            };

            // SortAggregate：输入按 group_by 排序 → 流式聚合，O(1) 组级内存
            if (agg->strategy == AggregatePlan::Strategy::Sort) {
                std::optional<Key> cur_key;
                std::vector<AggState> cur_states(agg->aggregates.size());
                auto child = execute_plan(ctx, agg->child.get());
                for (auto&& rec: child) {
                    Key key;
                    key.reserve(agg->group_by.size());
                    for (const auto& col: agg->group_by)
                        key.push_back(lookup_value(rec, col));
                    if (cur_key.has_value() && !KeyEq{}(*cur_key, key)) {
                        if (auto out = emit_group(*cur_key, cur_states))
                            co_yield *out;
                        cur_states.assign(agg->aggregates.size(), AggState{});
                    }
                    cur_key = key;
                    update_states(cur_states, rec);
                }
                if (cur_key.has_value()) {
                    if (auto out = emit_group(*cur_key, cur_states))
                        co_yield *out;
                }
                co_return;
            }

            // 处理分组聚合，使用哈希表
            std::unordered_map<Key, GroupData, KeyHash, KeyEq> groups;
            // 预分配哈希表容量，减少扩容开销
            groups.reserve(100);

            auto child = execute_plan(ctx, agg->child.get());
            // 处理输入记录，更新聚合状态
            for (auto&& rec: child) {
                Key key;
                key.reserve(agg->group_by.size());
                // 提取分组键
                for (const auto& col: agg->group_by) {
                    key.push_back(lookup_value(rec, col));
                }

                auto& entry = groups[key];
                if (entry.key.empty())
                    entry.key = key;
                if (entry.states.empty())
                    entry.states.resize(agg->aggregates.size());

                // 更新每个聚合函数的状态
                for (std::size_t i = 0; i < agg->aggregates.size(); ++i) {
                    auto& a = agg->aggregates[i];
                    auto& st = entry.states[i];
                    switch (a.func) {
                        case AggFunc::Count:
                            if (!a.arg.has_value()) {
                                ++st.count;
                            } else {
                                const auto& v = lookup_value(rec, *a.arg);
                                if (!std::holds_alternative<NullValue>(v))
                                    ++st.count;
                            }
                            break;
                        case AggFunc::Sum:
                        case AggFunc::Avg: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] SUM/AVG requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!std::holds_alternative<int64_t>(v)) {
                                throw std::runtime_error("[Executor] SUM/AVG only supports int64 values");
                            }
                            st.sum += std::get<int64_t>(v);
                            ++st.count;
                            break;
                        }
                        case AggFunc::Min: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] MIN requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!st.min.has_value() || compare_order(v, *st.min) < 0)
                                st.min = v;
                            break;
                        }
                        case AggFunc::Max: {
                            if (!a.arg.has_value())
                                throw std::runtime_error("[Executor] MAX requires a column argument");
                            const auto& v = lookup_value(rec, *a.arg);
                            if (std::holds_alternative<NullValue>(v))
                                break;
                            if (!st.max.has_value() || compare_order(v, *st.max) > 0)
                                st.max = v;
                            break;
                        }
                    }
                }
            }


            // 处理分组聚合
            for (auto& [key, data]: groups) {
                Record out;
                out.values.reserve(agg->projections.size());
                out.bindings.reserve(agg->projections.size());

                // 查找分组列的索引
                auto find_group_index = [&](const ColumnRef& col) -> std::size_t {
                    for (std::size_t i = 0; i < agg->group_by.size(); ++i) {
                        const auto& g = agg->group_by[i];
                        if ((g.table.empty() || g.table == col.table) && g.name == col.name)
                            return i;
                    }
                    throw std::runtime_error("[Executor] Column not part of GROUP BY: " + col.name);
                };

                // 生成输出记录
                for (std::size_t i = 0; i < agg->projections.size(); ++i) {
                    const auto& item = agg->projections[i];
                    if (std::holds_alternative<AggregateExpr>(item.value)) {
                        const auto& a = std::get<AggregateExpr>(item.value);
                        auto pos = agg_index(a);
                        out.values.push_back(finalize(data.states[pos], a.func));
                        std::string name = item.alias.has_value() ? *item.alias : (a.arg ? a.arg->name : "count");
                        std::string table = a.arg ? a.arg->table : "";
                        out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                    } else {
                        const auto& expr = std::get<Expression>(item.value);
                        if (!std::holds_alternative<ColumnRef>(expr)) {
                            throw std::runtime_error(
                                    "[Executor] Only GROUP BY columns allowed as non-aggregate projections");
                        }
                        const auto& col = std::get<ColumnRef>(expr);
                        auto idx = find_group_index(col);
                        out.values.push_back(data.key[idx]);
                        std::string name = item.alias.has_value() ? *item.alias : col.name;
                        out.bindings.push_back(Binding{ col.table, name, out.bindings.size() });
                    }
                }

                // 若存在 HAVING，将 aggregates 中不在 projections 的聚合值追加到 Record，
                // 确保 evaluate_bool_expr 能在 Record 中找到 HAVING 引用的聚合结果。
                if (agg->having.has_value()) {
                    for (std::size_t ai = 0; ai < agg->aggregates.size(); ++ai) {
                        const auto& a = agg->aggregates[ai];
                        bool in_proj = false;
                        for (const auto& item : agg->projections) {
                            if (auto* pa = std::get_if<AggregateExpr>(&item.value)) {
                                if (pa->func == a.func &&
                                    ((pa->arg.has_value() && a.arg.has_value() && pa->arg->name == a.arg->name) ||
                                     (!pa->arg.has_value() && !a.arg.has_value()))) {
                                    in_proj = true; break;
                                }
                            }
                        }
                        if (!in_proj) {
                            out.values.push_back(finalize(data.states[ai], a.func));
                            std::string name = a.arg ? a.arg->name : "count";
                            std::string table = a.arg ? a.arg->table : "";
                            out.bindings.push_back(Binding{ table, name, out.bindings.size() });
                        }
                    }
                    if (evaluate_bool_expr(*agg->having, out) != SqlBool::True)
                        continue;
                }
                co_yield out;
            }
            co_return;
        }

        // 处理插入计划节点（支持多行插入）
        if (const auto* ins = dynamic_cast<const InsertPlan*>(plan)) {
            // 构建所有要插入的行
            std::vector<Row> batch_rows;
            batch_rows.reserve(ins->rows.size());

            for (const auto& row_values: ins->rows) {
                Row row;
                row.values.resize(ins->table->columns().size());
                for (std::size_t i = 0; i < ins->column_indexes.size(); ++i) {
                    row.values[ins->column_indexes[i]] = row_values[i];
                }
                batch_rows.push_back(std::move(row));
            }

            // 约束校验：NOT NULL + 类型族匹配（对本次 INSERT 的每一行）。
            for (const auto& r: batch_rows)
                validate_row_constraints(ins->table->columns(), r);

            // 事务路径：写入 TxnWriteBuffer，等 COMMIT 时才落到 Table::rows_
            if (auto* sess = ctx.session.get(); sess && sess->in_transaction()) {
                auto& tbuf = sess->write_buffer.for_table(ins->table->name());
                for (auto& r: batch_rows) {
                    if (r.values.empty()) {
                        // 无主键列的表：直接落表（不参与 ROLLBACK）。
                        ins->table->insert_batch({ std::move(r) });
                        continue;
                    }
                    Value pk = ins->table->row_key(r);
                    // 主键唯一性：本事务已删除该 pk 则允许重插；否则缓冲已有或已提交可见即为重复键。
                    if (tbuf.deletes.count(pk) == 0) {
                        if (tbuf.upserts.count(pk) > 0)
                            throw std::runtime_error("ERROR: duplicate key in transaction");
                        if (ins->table->lookup_visible(pk, std::numeric_limits<uint64_t>::max()).has_value())
                            throw std::runtime_error("ERROR: duplicate key violates primary key");
                    }
                    // 写写冲突检测失败时立即中止当前事务。
                    txn_acquire_row_lock(ctx, sess->current_txn_id, ins->table->name(), pk);
                    tbuf.deletes.erase(pk);
                    tbuf.upserts[pk] = std::move(r);
                }
                co_return;
            }

            // 自动提交路径：主键唯一性校验（本批次内部 + 与已提交数据），再批量插入。
            {
                std::unordered_set<Value, ValueHash, ValueEq> seen_pks;
                for (const auto& r: batch_rows) {
                    auto pk = row_pk(*ins->table, r);
                    if (!pk.has_value())
                        continue;
                    if (!seen_pks.insert(*pk).second)
                        throw std::runtime_error("ERROR: duplicate key in INSERT");
                    if (ins->table->lookup_visible(*pk, std::numeric_limits<uint64_t>::max()).has_value())
                        throw std::runtime_error("ERROR: duplicate key violates primary key");
                }
            }
            // 使用批量插入（对支持优化的存储引擎更高效）
            uint64_t ts = 0;
            if (auto* sess = ctx.session.get())
                ts = sess->auto_commit_ts;
            ins->table->insert_batch(std::move(batch_rows), ts);
            // 自动提交语句写完全部行后写入全局提交日志（原子提交点），保证崩溃恢复的原子性。
            if (ts != 0 && ctx.storage)
                ctx.storage->mark_committed(ts);
            co_return;
        }

        // 处理更新计划节点
        if (const auto* upd = dynamic_cast<const UpdatePlan*>(plan)) {
            auto* sess = ctx.session.get();
            const bool in_txn = sess && sess->in_transaction();

            if (in_txn) {
                // 事务路径：在 TxnWriteBuffer 上更新 —— 既要扫已 commit 的 rows_，
                // 也要扫 buffer 里 upserts 的 pk，使 read-your-own-writes 一致。
                auto& tbuf = sess->write_buffer.for_table(upd->table->name());

                auto apply_row_update = [&](const Row& base_row) -> std::optional<Row> {
                    Record rec = make_record_from_row(*upd->table, base_row, upd->table->name());
                    if (upd->where.has_value() && evaluate_bool_expr(*upd->where, rec) != SqlBool::True)
                        return std::nullopt;
                    Row new_row = base_row;
                    for (const auto& a: upd->assignments) {
                        Value v = std::holds_alternative<Literal>(a.value) ? std::get<Literal>(a.value).value
                                                                           : eval_expression(rec, a.value);
                        new_row.values[a.column_index] = v;
                    }
                    validate_row_constraints(upd->table->columns(), new_row);
                    return new_row;
                };

                // 基线可见行：存储型走 scan_visible（去除 rows_ 常驻），纯内存表读 rows_。
                std::vector<Row> base = upd->table->has_storage()
                                                ? upd->table->scan_visible(std::numeric_limits<uint64_t>::max())
                                                : upd->table->rows();
                // 扫基线：跳过 buffer 标删的 pk，buffer 里有 upsert 的 pk 用 buffer 版
                for (const auto& base_row: base) {
                    if (base_row.values.empty())
                        continue;
                    Value pk = upd->table->row_key(base_row);
                    if (tbuf.deletes.count(pk))
                        continue;
                    auto upsert_it = tbuf.upserts.find(pk);
                    const Row& cur = upsert_it != tbuf.upserts.end() ? upsert_it->second : base_row;
                    if (auto nr = apply_row_update(cur)) {
                        // 先申请行锁，避免并发事务覆盖同一主键。
                        txn_acquire_row_lock(ctx, sess->current_txn_id, upd->table->name(), pk);
                        tbuf.upserts[pk] = std::move(*nr);
                    }
                }
                // 还要扫 buffer 里"纯 INSERT"（pk 不在基线）的条目：避免 update 漏掉自己刚 INSERT 的行
                std::vector<Value> pure_inserts;
                {
                    std::unordered_set<Value, ValueHash, ValueEq> rows_pk;
                    for (const auto& r: base) {
                        if (!r.values.empty())
                            rows_pk.insert(upd->table->row_key(r));
                    }
                    for (const auto& [pk, _]: tbuf.upserts) {
                        if (!rows_pk.count(pk))
                            pure_inserts.push_back(pk);
                    }
                }
                for (const Value& pk: pure_inserts) {
                    auto it = tbuf.upserts.find(pk);
                    if (it == tbuf.upserts.end())
                        continue;
                    if (auto nr = apply_row_update(it->second)) {
                        tbuf.upserts[pk] = std::move(*nr);
                    }
                }
                co_return;
            }

            // 自动提交路径
            uint64_t ts = sess ? sess->auto_commit_ts : 0;
            if (upd->table->has_storage()) {
                // 存储型：从可见快照计算更新行并持久化，不再维护 rows_。
                auto base = upd->table->scan_visible(std::numeric_limits<uint64_t>::max());
                for (const auto& row: base) {
                    Record rec = make_record_from_row(*upd->table, row, upd->table->name());
                    if (upd->where.has_value() && evaluate_bool_expr(*upd->where, rec) != SqlBool::True)
                        continue;
                    Row new_row = row;
                    for (const auto& a: upd->assignments) {
                        Value v = std::holds_alternative<Literal>(a.value) ? std::get<Literal>(a.value).value
                                                                           : eval_expression(rec, a.value);
                        validate_value(upd->table->columns()[a.column_index], v);
                        new_row.values[a.column_index] = std::move(v);
                    }
                    upd->table->persist_row_upsert(new_row, ts);
                }
            } else {
                // 纯内存表：就地修改 rows_。
                auto& rows = upd->table->rows_mut();
                for (auto& row: rows) {
                    Record rec = make_record_from_row(*upd->table, row, upd->table->name());
                    if (upd->where.has_value() && evaluate_bool_expr(*upd->where, rec) != SqlBool::True)
                        continue;
                    std::vector<std::pair<std::size_t, Value>> pending;
                    pending.reserve(upd->assignments.size());
                    for (const auto& a: upd->assignments) {
                        Value v = std::holds_alternative<Literal>(a.value) ? std::get<Literal>(a.value).value
                                                                           : eval_expression(rec, a.value);
                        validate_value(upd->table->columns()[a.column_index], v);
                        pending.emplace_back(a.column_index, std::move(v));
                    }
                    for (auto& [idx, v]: pending)
                        row.values[idx] = std::move(v);
                }
            }
            upd->table->refresh_indexes();
            // 自动提交语句写完全部行后写入全局提交日志（原子提交点），保证崩溃恢复的原子性。
            if (ts != 0 && ctx.storage)
                ctx.storage->mark_committed(ts);
            co_return;
        }

        // 处理删除计划节点
        if (const auto* del = dynamic_cast<const DeletePlan*>(plan)) {
            auto* sess = ctx.session.get();
            const bool in_txn = sess && sess->in_transaction();

            if (in_txn) {
                auto& tbuf = sess->write_buffer.for_table(del->table->name());

                auto matches = [&](const Row& row) -> bool {
                    if (!del->where.has_value())
                        return true;
                    Record rec = make_record_from_row(*del->table, row, del->table->name());
                    return evaluate_bool_expr(*del->where, rec) == SqlBool::True;
                };

                // 基线可见行：存储型走 scan_visible，纯内存表读 rows_。
                std::vector<Row> base = del->table->has_storage()
                                                ? del->table->scan_visible(std::numeric_limits<uint64_t>::max())
                                                : del->table->rows();
                // 扫基线：标记 deletes
                for (const auto& base_row: base) {
                    if (base_row.values.empty())
                        continue;
                    Value pk = del->table->row_key(base_row);
                    if (tbuf.deletes.count(pk))
                        continue;
                    auto upsert_it = tbuf.upserts.find(pk);
                    const Row& cur = upsert_it != tbuf.upserts.end() ? upsert_it->second : base_row;
                    if (matches(cur)) {
                        // 先申请行锁，避免并发事务覆盖同一主键。
                        txn_acquire_row_lock(ctx, sess->current_txn_id, del->table->name(), pk);
                        tbuf.upserts.erase(pk);
                        tbuf.deletes.insert(pk);
                    }
                }
                // 扫 buffer 里纯 INSERT 的行
                std::vector<Value> to_remove_inserts;
                {
                    std::unordered_set<Value, ValueHash, ValueEq> rows_pk;
                    for (const auto& r: base) {
                        if (!r.values.empty())
                            rows_pk.insert(del->table->row_key(r));
                    }
                    for (const auto& [pk, row]: tbuf.upserts) {
                        if (!rows_pk.count(pk) && matches(row))
                            to_remove_inserts.push_back(pk);
                    }
                }
                for (const Value& pk: to_remove_inserts) {
                    tbuf.upserts.erase(pk);
                    // 纯 INSERT 撤销不需要写 deletes（rows_ 中本就没有这条）
                }
                co_return;
            }

            // 自动提交路径
            uint64_t ts = sess ? sess->auto_commit_ts : 0;
            std::vector<Value> deleted_keys;
            if (del->table->has_storage()) {
                // 存储型：从可见快照筛出匹配行，写 tombstone 持久化（不再维护 rows_）。
                auto base = del->table->scan_visible(std::numeric_limits<uint64_t>::max());
                for (const auto& row: base) {
                    if (del->where.has_value()) {
                        Record rec = make_record_from_row(*del->table, row, del->table->name());
                        if (evaluate_bool_expr(*del->where, rec) != SqlBool::True)
                            continue;
                    }
                    if (!row.values.empty())
                        deleted_keys.push_back(del->table->row_key(row));
                }
            } else {
                // 纯内存表：就地从 rows_ 删除。
                auto& rows = del->table->rows_mut();
                std::erase_if(rows, [&](const Row& row) -> bool {
                    if (del->where.has_value()) {
                        Record rec = make_record_from_row(*del->table, row, del->table->name());
                        if (evaluate_bool_expr(*del->where, rec) != SqlBool::True)
                            return false;
                    }
                    if (!row.values.empty())
                        deleted_keys.push_back(del->table->row_key(row));
                    return true;
                });
            }
            for (const Value& k: deleted_keys)
                del->table->persist_row_delete(k, ts);
            del->table->refresh_indexes();
            // 自动提交语句写完全部删除后写入全局提交日志（原子提交点），保证崩溃恢复的原子性。
            if (ts != 0 && ctx.storage)
                ctx.storage->mark_committed(ts);
            co_return;
        }

        throw std::runtime_error("[Executor] Unsupported plan node type");
    }

    // ──────────────────────────────────────────────────────────────────────
    //  Executor 类的公开接口（PG 风格 execMain）
    // ──────────────────────────────────────────────────────────────────────

    std::generator<Record> Executor::run(const PlanNode* plan) {
        ExecutionContext ctx = ctx_;
        for (auto&& rec: execute_plan(ctx, plan)) {
            co_yield rec;
        }
    }

    namespace {
        const char* plan_node_name(const PlanNode* p) {
            if (dynamic_cast<const SeqScanPlan*>(p))
                return "SeqScan";
            if (dynamic_cast<const IndexScanPlan*>(p))
                return "IndexScan";
            if (dynamic_cast<const FilterPlan*>(p))
                return "Filter";
            if (dynamic_cast<const ProjectPlan*>(p))
                return "Project";
            if (dynamic_cast<const HashJoinPlan*>(p))
                return "HashJoin";
            if (dynamic_cast<const MergeJoinPlan*>(p))
                return "MergeJoin";
            if (dynamic_cast<const NestedLoopJoinPlan*>(p))
                return "NestedLoopJoin";
            if (dynamic_cast<const AggregatePlan*>(p))
                return "Aggregate";
            if (dynamic_cast<const OrderByPlan*>(p))
                return "OrderBy";
            if (dynamic_cast<const LimitPlan*>(p))
                return "Limit";
            if (dynamic_cast<const InsertPlan*>(p))
                return "Insert";
            if (dynamic_cast<const UpdatePlan*>(p))
                return "Update";
            if (dynamic_cast<const DeletePlan*>(p))
                return "Delete";
            return "Other";
        }

        // 遍历计划树收集算子名称。
        void collect_plan_nodes(const PlanNode* p, std::vector<std::string>& out) {
            if (!p)
                return;
            out.push_back(plan_node_name(p));
            // 根据算子类型递归遍历子节点。
            if (auto* n = dynamic_cast<const SeqScanPlan*>(p))
                return;
            if (auto* n = dynamic_cast<const IndexScanPlan*>(p))
                return;
            if (auto* n = dynamic_cast<const FilterPlan*>(p)) {
                collect_plan_nodes(n->child.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const ProjectPlan*>(p)) {
                collect_plan_nodes(n->child.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const HashJoinPlan*>(p)) {
                collect_plan_nodes(n->left.get(), out);
                collect_plan_nodes(n->right.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const MergeJoinPlan*>(p)) {
                collect_plan_nodes(n->left.get(), out);
                collect_plan_nodes(n->right.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const NestedLoopJoinPlan*>(p)) {
                collect_plan_nodes(n->left.get(), out);
                collect_plan_nodes(n->right.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const AggregatePlan*>(p)) {
                collect_plan_nodes(n->child.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const OrderByPlan*>(p)) {
                collect_plan_nodes(n->child.get(), out);
                return;
            }
            if (auto* n = dynamic_cast<const LimitPlan*>(p)) {
                collect_plan_nodes(n->child.get(), out);
                return;
            }
            // DML plans
            if (dynamic_cast<const InsertPlan*>(p))
                return;
            if (dynamic_cast<const UpdatePlan*>(p))
                return;
            if (dynamic_cast<const DeletePlan*>(p))
                return;
        }
    } // namespace

    std::generator<Record> Executor::run_profiled(const PlanNode* plan, QueryStats& stats) {
        // Pre-populate operator names from the plan tree.
        {
            std::vector<std::string> nodes;
            collect_plan_nodes(plan, nodes);
            for (auto& n: nodes) {
                stats.add(n, 0, 0.0);
            }
        }

        ExecutionContext ctx = ctx_;
        auto t0 = std::chrono::steady_clock::now();
        std::size_t total_rows = 0;
        for (auto&& rec: execute_plan(ctx, plan)) {
            ++total_rows;
            co_yield rec;
        }
        stats.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!stats.operators.empty()) {
            stats.operators.front().rows = total_rows;
            stats.operators.front().elapsed_ms = stats.total_ms;
        }
    }

} // namespace corodb
