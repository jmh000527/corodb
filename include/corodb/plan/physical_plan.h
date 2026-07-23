// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file physical_plan.h @brief 物理计划节点定义。 */

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "corodb/ast/ast.h"
#include "corodb/storage/table.h"

namespace corodb {

    // 前向声明
    class StorageEngine;

    /** @brief 列绑定信息，追踪查询结果中每列的来源与位置。 */
    struct Binding {
        std::string table;    ///< 来源表名
        std::string column;   ///< 列名
        std::size_t index;    ///< 在记录中的位置索引
        Oid table_oid{ 0 };   ///< 表的对象标识符
        RtIndex rtindex{ 0 }; ///< 范围表索引（用于区分同一表的多个引用）

        /** @brief 获取完全限定列名。 */
        [[nodiscard]] std::string qualified_name() const {
            return table.empty() ? column : table + "." + column;
        }

        /** @brief 检查是否匹配给定的列引用。 */
        [[nodiscard]] bool matches(const ColumnRef& ref) const {
            if (!ref.table.empty() && ref.table != table) {
                return false;
            }
            return ref.name == column;
        }
    };

    /** @brief 执行过程中的一行数据，含值列表与绑定信息。 */
    struct Record {
        std::vector<Value> values;     ///< 记录的值列表
        std::vector<Binding> bindings; ///< 每个值的绑定信息

        /** @brief 获取记录中值的数量。 */
        [[nodiscard]] std::size_t size() const noexcept {
            return values.size();
        }

        /** @brief 检查记录是否为空。 */
        [[nodiscard]] bool empty() const noexcept {
            return values.empty();
        }

        /** @brief 按列名查找值。
         *  @param column 列名。
         *  @param table  表名（可选）。
         *  @return 找到的值，或 std::nullopt。 */
        [[nodiscard]] std::optional<Value> find_value(const std::string& column, const std::string& table = {}) const {
            for (std::size_t i = 0; i < bindings.size(); ++i) {
                if (bindings[i].column == column) {
                    if (table.empty() || bindings[i].table == table) {
                        return values[i];
                    }
                }
            }
            return std::nullopt;
        }
    };

    /** @brief EXPLAIN ANALYZE 单算子运行时统计。 */
    struct OperatorStats {
        std::string name;          ///< 算子类型名称
        std::size_t rows{ 0 };     ///< 产出行数
        double elapsed_ms{ 0.0 }; ///< 墙钟耗时（毫秒）
    };

    /** @brief 全查询的算子统计集合。 */
    struct QueryStats {
        std::vector<OperatorStats> operators;
        double total_ms{ 0.0 };

        void add(const std::string& op_name, std::size_t rows, double ms) {
            operators.push_back(OperatorStats{ op_name, rows, ms });
        }
    };

    /** @brief 所有物理计划节点的抽象基类。 */
    struct PlanNode {
        /// 虚析构函数，确保子类能正确析构
        virtual ~PlanNode() = default;

        // 禁用复制（计划节点通常通过unique_ptr管理）
        PlanNode(const PlanNode&) = delete;
        PlanNode& operator=(const PlanNode&) = delete;

        // 允许移动
        PlanNode(PlanNode&&) = default;
        PlanNode& operator=(PlanNode&&) = default;

    protected:
        /// 受保护的默认构造函数
        PlanNode() = default;
    };

    /** @brief 全表顺序扫描计划节点。 */
    struct SeqScanPlan : PlanNode {
        /** @brief 构造顺序扫描计划节点。 */
        explicit SeqScanPlan(std::shared_ptr<Table> t, std::string alias_name = {})
            : table(std::move(t)), alias(std::move(alias_name)) {
        }

        std::shared_ptr<Table> table; ///< 要扫描的表
        std::string alias;            ///< 表的别名（空则使用表名）
    };

    /** @brief 索引扫描计划节点，按键值快速定位行（支持等值与范围）。 */
    struct IndexScanPlan : PlanNode {
        /** @brief 构造等值索引扫描计划节点。 */
        IndexScanPlan(std::shared_ptr<Table> t, std::string alias_name, std::string col, Value key)
            : table(std::move(t)), alias(std::move(alias_name)), column(std::move(col)), key(std::move(key)) {
        }

        /** @brief 构造范围索引扫描计划节点（low/high 为 nullopt 表示该侧无界）。 */
        IndexScanPlan(std::shared_ptr<Table> t, std::string alias_name, std::string col,
                      std::optional<Value> low_bound, bool low_inc, std::optional<Value> high_bound, bool high_inc)
            : table(std::move(t)), alias(std::move(alias_name)), column(std::move(col)), is_range(true),
              low(std::move(low_bound)), low_inclusive(low_inc), high(std::move(high_bound)),
              high_inclusive(high_inc) {
        }

        std::shared_ptr<Table> table;    ///< 要扫描的表
        std::string alias;               ///< 表的别名
        std::string column;              ///< 索引列名
        Value key;                       ///< 等值键（is_range=false 时使用）
        bool is_range{ false };          ///< 是否为范围扫描
        std::optional<Value> low;        ///< 范围下界（nullopt=无下界）
        bool low_inclusive{ false };     ///< 下界是否含等
        std::optional<Value> high;       ///< 范围上界（nullopt=无上界）
        bool high_inclusive{ false };    ///< 上界是否含等
    };

    /** @brief 过滤计划节点，保留满足谓词的记录。 */
    struct FilterPlan : PlanNode {
        /** @brief 构造过滤计划节点。 */
        FilterPlan(std::unique_ptr<PlanNode> c, BoolExpr p) : child(std::move(c)), predicate(std::move(p)) {
        }

        std::unique_ptr<PlanNode> child; ///< 子计划节点
        BoolExpr predicate;              ///< 过滤条件
    };

    /** @brief 投影计划节点，可选支持 DISTINCT 去重。 */
    struct ProjectPlan : PlanNode {
        /** @brief 构造投影计划节点。 */
        ProjectPlan(std::unique_ptr<PlanNode> c, std::vector<SelectStmt::SelectItem> cols, bool is_distinct = false)
            : child(std::move(c)), columns(std::move(cols)), distinct(is_distinct) {
        }

        std::unique_ptr<PlanNode> child;             ///< 子计划节点
        std::vector<SelectStmt::SelectItem> columns; ///< 投影列列表
        bool distinct{ false };                      ///< DISTINCT 标志
    };

    /** @brief 哈希连接计划节点，适用于等值连接。 */
    struct HashJoinPlan : PlanNode {
        /** @brief 构造哈希连接计划节点。 */
        HashJoinPlan(std::unique_ptr<PlanNode> l, std::unique_ptr<PlanNode> r, ColumnRef lkey, ColumnRef rkey,
                     std::optional<BoolExpr> residual = std::nullopt, JoinType jt = JoinType::Inner)
            : left(std::move(l)), right(std::move(r)), left_key(std::move(lkey)), right_key(std::move(rkey)),
              residual(std::move(residual)), type(jt) {
        }

        std::unique_ptr<PlanNode> left;   ///< 左子计划节点（构建端）
        std::unique_ptr<PlanNode> right;  ///< 右子计划节点（探测端）
        ColumnRef left_key;               ///< 左连接键
        ColumnRef right_key;              ///< 右连接键
        std::optional<BoolExpr> residual; ///< 残留条件（非等值条件）
        JoinType type{ JoinType::Inner }; ///< 连接类型
    };

    /** @brief 归并连接计划节点，要求输入已按连接键排序。 */
    struct MergeJoinPlan : PlanNode {
        /** @brief 构造归并连接计划节点。 */
        MergeJoinPlan(std::unique_ptr<PlanNode> l, std::unique_ptr<PlanNode> r, ColumnRef lkey, ColumnRef rkey,
                      std::optional<BoolExpr> residual = std::nullopt, JoinType jt = JoinType::Inner)
            : left(std::move(l)), right(std::move(r)), left_key(std::move(lkey)), right_key(std::move(rkey)),
              residual(std::move(residual)), type(jt) {
        }

        std::unique_ptr<PlanNode> left;   ///< 左子计划节点
        std::unique_ptr<PlanNode> right;  ///< 右子计划节点
        ColumnRef left_key;               ///< 左连接键
        ColumnRef right_key;              ///< 右连接键
        std::optional<BoolExpr> residual; ///< 残留条件
        JoinType type{ JoinType::Inner }; ///< 连接类型
        bool left_sorted{ false };        ///< 左子节点输出已按 left_key 排序
        bool right_sorted{ false };       ///< 右子节点输出已按 right_key 排序
    };

    /** @brief 聚合计划节点，支持 GROUP BY 与 HAVING。 */
    struct AggregatePlan : PlanNode {
        /** @brief 聚合算法策略。 */
        enum class Strategy {
            Hash, ///< 哈希分组，所有组在内存中并存。
            Sort  ///< 排序聚合，流式处理，O(1) 内存。
        };

        /** @brief 构造聚合计划节点。 */
        AggregatePlan(std::unique_ptr<PlanNode> c, std::vector<ColumnRef> group, std::vector<AggregateExpr> aggs,
                      std::vector<SelectStmt::SelectItem> proj, std::optional<BoolExpr> having,
                      Strategy strat = Strategy::Hash)
            : child(std::move(c)), group_by(std::move(group)), aggregates(std::move(aggs)),
              projections(std::move(proj)), having(std::move(having)), strategy(strat) {
        }

        std::unique_ptr<PlanNode> child;                 ///< 子计划节点
        std::vector<ColumnRef> group_by;                 ///< GROUP BY列列表
        std::vector<AggregateExpr> aggregates;           ///< 聚合表达式列表
        std::vector<SelectStmt::SelectItem> projections; ///< 投影列列表
        std::optional<BoolExpr> having;                  ///< HAVING条件
        Strategy strategy{ Strategy::Hash };             ///< Hash 或 Sort
    };

    /** @brief 嵌套循环连接计划节点，支持任意连接条件。 */
    struct NestedLoopJoinPlan : PlanNode {
        /** @brief 构造嵌套循环连接计划节点。 */
        NestedLoopJoinPlan(std::unique_ptr<PlanNode> l, std::unique_ptr<PlanNode> r, BoolExpr p,
                           JoinType jt = JoinType::Inner)
            : left(std::move(l)), right(std::move(r)), on(std::move(p)), type(jt) {
        }

        std::unique_ptr<PlanNode> left;   ///< 左子计划节点（外层）
        std::unique_ptr<PlanNode> right;  ///< 右子计划节点（内层）
        BoolExpr on;                      ///< 连接条件
        JoinType type{ JoinType::Inner }; ///< 连接类型
    };

    /** @brief 排序计划节点，支持多列排序与 Top-N 优化。 */
    struct OrderByPlan : PlanNode {
        /** @brief 构造排序计划节点。 */
        OrderByPlan(std::unique_ptr<PlanNode> c, std::vector<SelectStmt::OrderByItem> items,
                    std::optional<int64_t> lim = std::nullopt, std::optional<int64_t> off = std::nullopt)
            : child(std::move(c)), items(std::move(items)), limit(lim), offset(off) {
        }

        std::unique_ptr<PlanNode> child;            ///< 子计划节点
        std::vector<SelectStmt::OrderByItem> items; ///< 排序项列表
        std::optional<int64_t> limit;               ///< Top-N 优化的 LIMIT 值
        std::optional<int64_t> offset;              ///< Top-N 优化的 OFFSET 值
    };

    /** @brief 限制计划节点，实现 LIMIT/OFFSET 功能。 */
    struct LimitPlan : PlanNode {
        /** @brief 构造限制计划节点。 */
        LimitPlan(std::unique_ptr<PlanNode> c, std::optional<int64_t> lim, std::optional<int64_t> off)
            : child(std::move(c)), limit(lim), offset(off) {
        }

        std::unique_ptr<PlanNode> child; ///< 子计划节点
        std::optional<int64_t> limit;    ///< LIMIT值
        std::optional<int64_t> offset;   ///< OFFSET值
    };

    /** @brief 插入计划节点。 */
    struct InsertPlan : PlanNode {
        /** @brief 构造插入计划节点（单行）。 */
        InsertPlan(std::shared_ptr<Table> t, std::vector<std::size_t> idx, std::vector<Value> vals)
            : table(std::move(t)), column_indexes(std::move(idx)), rows({ std::move(vals) }) {
        }

        /** @brief 构造插入计划节点（多行）。 */
        InsertPlan(std::shared_ptr<Table> t, std::vector<std::size_t> idx, std::vector<std::vector<Value>> row_values)
            : table(std::move(t)), column_indexes(std::move(idx)), rows(std::move(row_values)) {
        }

        std::shared_ptr<Table> table;            ///< 目标表
        std::vector<std::size_t> column_indexes; ///< 列索引列表
        std::vector<std::vector<Value>> rows;    ///< 要插入的多行值列表
    };

    /** @brief 更新计划节点。 */
    struct UpdatePlan : PlanNode {
        /** @brief 更新赋值结构，表示 SET 子句中的一个赋值操作。 */
        struct Assignment {
            std::size_t column_index; ///< 要更新的列索引
            Expression value;         ///< 新值表达式
        };

        /** @brief 构造更新计划节点。 */
        UpdatePlan(std::shared_ptr<Table> t, std::vector<Assignment> assigns, std::optional<BoolExpr> where)
            : table(std::move(t)), assignments(std::move(assigns)), where(std::move(where)) {
        }

        std::shared_ptr<Table> table;        ///< 目标表
        std::vector<Assignment> assignments; ///< 赋值操作列表
        std::optional<BoolExpr> where;       ///< WHERE条件
    };

    /** @brief 删除计划节点。 */
    struct DeletePlan : PlanNode {
        /** @brief 构造删除计划节点。 */
        DeletePlan(std::shared_ptr<Table> t, std::optional<BoolExpr> where)
            : table(std::move(t)), where(std::move(where)) {
        }

        std::shared_ptr<Table> table;  ///< 目标表
        std::optional<BoolExpr> where; ///< WHERE条件
    };

    /** @brief 创建表计划节点。 */
    struct CreateTablePlan : PlanNode {
        /** @brief 构造创建表计划节点。 */
        CreateTablePlan(std::string name, std::vector<Column> cols, Catalog* cat, StorageEngine* storage)
            : table(std::move(name)), columns(std::move(cols)), catalog(cat), storage(storage) {
        }

        std::string table;                 ///< 新表名称
        std::vector<Column> columns;       ///< 列定义列表
        Catalog* catalog{ nullptr };       ///< 目录指针
        StorageEngine* storage{ nullptr }; ///< 存储引擎指针
    };

    /** @brief 创建索引计划节点。 */
    struct CreateIndexPlan : PlanNode {
        /** @brief 构造创建索引计划节点。 */
        CreateIndexPlan(std::shared_ptr<Table> t, std::string idx_name, std::string col)
            : table(std::move(t)), index_name(std::move(idx_name)), column(std::move(col)) {
        }

        std::shared_ptr<Table> table; ///< 目标表
        std::string index_name;       ///< 索引名称
        std::string column;           ///< 要索引的列名
    };

    /** @brief 删除表计划节点。 */
    struct DropTablePlan : PlanNode {
        /** @brief 构造删除表计划节点。 */
        DropTablePlan(std::string name, bool if_exists, Catalog* cat, StorageEngine* storage)
            : table(std::move(name)), if_exists(if_exists), catalog(cat), storage(storage) {
        }

        std::string table;                 ///< 要删除的表名
        bool if_exists{ false };           ///< IF EXISTS 标志
        Catalog* catalog{ nullptr };       ///< 目录指针
        StorageEngine* storage{ nullptr }; ///< 存储引擎指针
    };

    /** @brief 删除索引计划节点。 */
    struct DropIndexPlan : PlanNode {
        /** @brief 构造删除索引计划节点。 */
        DropIndexPlan(std::shared_ptr<Table> t, std::string idx_name, bool if_exists)
            : table(std::move(t)), index_name(std::move(idx_name)), if_exists(if_exists) {
        }

        std::shared_ptr<Table> table; ///< 目标表
        std::string index_name;       ///< 要删除的索引名
        bool if_exists{ false };      ///< IF EXISTS 标志
    };

} // namespace corodb
