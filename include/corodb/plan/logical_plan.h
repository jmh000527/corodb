// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file logical_plan.h @brief 逻辑计划节点定义。 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "corodb/ast/ast.h"
#include "corodb/storage/table.h"

namespace corodb::opt {

    /** @brief 逻辑计划节点类型。 */
    enum class LogicalKind {
        Scan,
        Filter,
        Project,
        Join,
        Aggregate,
        Sort,
        Limit,
        Values,
        DML,
        DDL,
    };

    struct LogicalPlan;
    using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

    /** @brief 投影列。 */
    struct LogicalColumn {
        Expression expr;
        std::string output_name;
    };

    /** @brief 排序键。 */
    struct LogicalSortKey {
        Expression expr;
        bool ascending{ true };
    };

    /** @brief 聚合项。 */
    struct LogicalAggregateItem {
        AggFunc func{ AggFunc::Count };
        std::optional<Expression> arg;
        bool distinct{ false };
        std::string output_name;
    };

    /** @brief 扫描节点载荷。 */
    struct LogicalScan {
        std::shared_ptr<Table> table;
        std::string alias;
    };

    /** @brief 过滤节点载荷——WHERE 子句的条件过滤。 */
    struct LogicalFilter {
        BoolExpr predicate;   ///< 过滤条件（布尔表达式）
        LogicalPlanPtr child; ///< 子计划
    };

    /** @brief 投影节点载荷——SELECT 列的投影输出。 */
    struct LogicalProject {
        std::vector<LogicalColumn> columns; ///< 输出列列表
        LogicalPlanPtr child;               ///< 子计划
        bool distinct{ false };             ///< SELECT DISTINCT
    };

    /** @brief 连接节点载荷——JOIN 操作（支持 INNER/LEFT/RIGHT/FULL）。 */
    struct LogicalJoin {
        JoinType join_type{ JoinType::Inner }; ///< 连接类型
        std::optional<BoolExpr> on;            ///< ON 条件（可选）
        LogicalPlanPtr left;                   ///< 左子计划
        LogicalPlanPtr right;                  ///< 右子计划
    };

    /** @brief 聚合节点载荷——GROUP BY + 聚合函数 + HAVING。 */
    struct LogicalAggregate {
        std::vector<Expression> group_by;                ///< 分组列
        std::vector<LogicalAggregateItem> aggregates;    ///< 聚合表达式列表
        std::optional<BoolExpr> having;                  ///< HAVING 过滤条件
        LogicalPlanPtr child;                            ///< 子计划
    };

    /** @brief 排序节点载荷——ORDER BY 子句。 */
    struct LogicalSort {
        std::vector<LogicalSortKey> keys; ///< 排序键列表（多列排序）
        LogicalPlanPtr child;             ///< 子计划
    };

    /** @brief 限制节点载荷——LIMIT / OFFSET 子句。 */
    struct LogicalLimit {
        std::optional<std::size_t> offset; ///< 偏移量（OFFSET）
        std::optional<std::size_t> limit;  ///< 最大行数（LIMIT）
        LogicalPlanPtr child;              ///< 子计划
    };

    /** @brief 值节点载荷——INSERT 的常数值行。 */
    struct LogicalValues {
        std::vector<std::vector<Expression>> rows; ///< 值行列表
    };

    /** @brief DML 节点载荷——INSERT / UPDATE / DELETE 操作。 */
    struct LogicalDML {
        enum class Kind { Insert, Update, Delete }; ///< DML 操作类型
        Kind kind{ Kind::Insert };
        std::shared_ptr<Table> table;               ///< 目标表
        LogicalPlanPtr source;                      ///< 数据源（Scan 或 Values）
        std::vector<std::string> columns;           ///< 涉及的列名
        std::vector<Expression> set_exprs;          ///< UPDATE 的 SET 表达式
        std::optional<BoolExpr> where;              ///< WHERE 条件（UPDATE/DELETE）
    };

    /** @brief DDL 节点载荷——CREATE/DROP TABLE/INDEX，保留原始 AST。 */
    struct LogicalDDL {
        Statement original; ///< 原始 AST 语句
    };

    /** @brief 统一的逻辑计划节点容器。 */
    struct LogicalPlan {
        LogicalKind kind{ LogicalKind::Scan };
        std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalJoin, LogicalAggregate, LogicalSort,
                     LogicalLimit, LogicalValues, LogicalDML, LogicalDDL>
                node;

        template<class T>
        static LogicalPlanPtr make(LogicalKind k, T&& v) {
            auto p = std::make_unique<LogicalPlan>();
            p->kind = k;
            p->node = std::forward<T>(v);
            return p;
        }

        static LogicalPlanPtr make_scan(std::shared_ptr<Table> t, std::string alias = {}) {
            return make(LogicalKind::Scan, LogicalScan{ std::move(t), std::move(alias) });
        }
        static LogicalPlanPtr make_filter(BoolExpr pred, LogicalPlanPtr child) {
            return make(LogicalKind::Filter, LogicalFilter{ std::move(pred), std::move(child) });
        }
        static LogicalPlanPtr make_project(std::vector<LogicalColumn> cols, LogicalPlanPtr child) {
            return make(LogicalKind::Project, LogicalProject{ std::move(cols), std::move(child) });
        }
        static LogicalPlanPtr make_limit(std::optional<std::size_t> off, std::optional<std::size_t> lim,
                                         LogicalPlanPtr child) {
            return make(LogicalKind::Limit, LogicalLimit{ off, lim, std::move(child) });
        }
    };

    /** @brief 深拷贝逻辑计划树。 */
    [[nodiscard]] LogicalPlanPtr clone(const LogicalPlan& plan);
    /** @brief 生成便于调试的文本表示。 */
    [[nodiscard]] std::string to_string(const LogicalPlan& plan, int indent = 0);

} // namespace corodb::opt
