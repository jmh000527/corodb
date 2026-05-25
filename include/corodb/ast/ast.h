// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file ast.h @brief SQL 抽象语法树节点定义。 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "corodb/common/types.h"

namespace corodb {

    /** @brief 列引用，支持简单列名与 table.column 限定形式。 */
    struct ColumnRef {
        std::string table;    ///< 表名（可选，空字符串表示从上下文推断）
        std::string name;     ///< 列名
        Oid table_oid{ 0 };   ///< 表的对象标识符（语义分析后填充）
        RtIndex rtindex{ 0 }; ///< 范围表索引（用于区分同表的多个引用）

        /** @brief 获取完全限定名。
         *  @return 格式为 "table.column" 或仅 "column"。 */
        [[nodiscard]] std::string qualified_name() const {
            return table.empty() ? name : table + "." + name;
        }

        /** @brief 检查是否与另一个列引用匹配（考虑表名限定）。 */
        [[nodiscard]] bool matches(const ColumnRef& other) const {
            if (!table.empty() && !other.table.empty() && table != other.table) {
                return false;
            }
            return name == other.name;
        }

        /// 相等比较运算符
        [[nodiscard]] bool operator==(const ColumnRef& other) const {
            return table == other.table && name == other.name;
        }
    };

    /** @brief SQL 字面量，表示数字、字符串或 NULL 常量。 */
    struct Literal {
        Value value; ///< 字面量的实际值

        /** @brief 从整数构造字面量。 */
        static Literal from_int(int64_t v) {
            return Literal{ v };
        }

        /** @brief 从字符串构造字面量。 */
        static Literal from_string(std::string s) {
            return Literal{ std::move(s) };
        }

        /** @brief 构造 NULL 字面量。 */
        static Literal null() {
            return Literal{ NullValue{} };
        }

        /** @brief 检查是否为 NULL。 */
        [[nodiscard]] bool is_null() const {
            return std::holds_alternative<NullValue>(value);
        }
    };

    // 前向声明，用于定义Expression类型
    struct AggregateExpr;
    struct BinaryExpr;
    struct FunctionExpr;

    /** @brief SQL 表达式（列引用、字面量、二元运算、聚合或标量函数）。 */
    using Expression =
            std::variant<ColumnRef, Literal, std::shared_ptr<BinaryExpr>, AggregateExpr, std::shared_ptr<FunctionExpr>>;

    // 前向声明
    struct ExplainStmt;

    /** @brief 聚合函数类型。 */
    enum class AggFunc {
        Count, ///< COUNT - 计数
        Sum,   ///< SUM - 求和
        Avg,   ///< AVG - 平均值
        Min,   ///< MIN - 最小值
        Max    ///< MAX - 最大值
    };

    /** @brief 将聚合函数类型转为字符串。 */
    [[nodiscard]] inline const char* agg_func_name(AggFunc func) {
        switch (func) {
            case AggFunc::Count:
                return "COUNT";
            case AggFunc::Sum:
                return "SUM";
            case AggFunc::Avg:
                return "AVG";
            case AggFunc::Min:
                return "MIN";
            case AggFunc::Max:
                return "MAX";
            default:
                return "UNKNOWN";
        }
    }

    /** @brief 连接类型。 */
    enum class JoinType {
        Inner, ///< INNER JOIN - 内连接
        Left,  ///< LEFT JOIN - 左外连接
        Right, ///< RIGHT JOIN - 右外连接
        Full   ///< FULL JOIN - 全外连接
    };

    /** @brief 将连接类型转为字符串。 */
    [[nodiscard]] inline const char* join_type_name(JoinType type) {
        switch (type) {
            case JoinType::Inner:
                return "INNER";
            case JoinType::Left:
                return "LEFT";
            case JoinType::Right:
                return "RIGHT";
            case JoinType::Full:
                return "FULL";
            default:
                return "UNKNOWN";
        }
    }

    /** @brief 聚合函数表达式，如 COUNT(*)、SUM(column) 等。 */
    struct AggregateExpr {
        AggFunc func{ AggFunc::Count }; ///< 聚合函数类型
        std::optional<ColumnRef> arg;   ///< 参数列（nullopt表示COUNT(*)）

        /** @brief 检查是否为 COUNT(*)。 */
        [[nodiscard]] bool is_count_star() const noexcept {
            return func == AggFunc::Count && !arg.has_value();
        }

        /** @brief 获取聚合函数的字符串表示。 */
        [[nodiscard]] std::string to_string() const {
            std::string result = agg_func_name(func);
            result += "(";
            if (arg.has_value()) {
                result += arg->qualified_name();
            } else {
                result += "*";
            }
            result += ")";
            return result;
        }
    };

    /** @brief 二元算术运算表达式（加减乘除取模拼接）。 */
    struct BinaryExpr {
        /** @brief 二元运算符。 */
        enum class Op {
            Add,   ///< 加法 (+)
            Sub,   ///< 减法 (-)
            Mul,   ///< 乘法 (*)
            Div,   ///< 除法 (/)
            Mod,   ///< 取模 (%)
            Concat ///< 字符串连接 (||)
        };

        Op op{ Op::Add }; ///< 运算符类型
        Expression lhs;   ///< 左操作数
        Expression rhs;   ///< 右操作数

        /** @brief 获取运算符的字符串表示。 */
        [[nodiscard]] static const char* op_str(Op op) {
            switch (op) {
                case Op::Add:
                    return "+";
                case Op::Sub:
                    return "-";
                case Op::Mul:
                    return "*";
                case Op::Div:
                    return "/";
                case Op::Mod:
                    return "%";
                case Op::Concat:
                    return "||";
                default:
                    return "?";
            }
        }
    };

    /** @brief 标量函数类型。 */
    enum class ScalarFunc {
        Coalesce, ///< COALESCE - 返回第一个非NULL值
        Nullif,   ///< NULLIF - 如果两值相等返回NULL
        Upper,    ///< UPPER - 转大写
        Lower,    ///< LOWER - 转小写
        Length,   ///< LENGTH - 字符串长度
        Trim,     ///< TRIM - 去除首尾空白
        Substr,   ///< SUBSTR - 子字符串
        Abs,      ///< ABS - 绝对值
        // 可扩展更多函数
    };

    /** @brief 将标量函数类型转为字符串。 */
    [[nodiscard]] inline const char* scalar_func_name(ScalarFunc func) {
        switch (func) {
            case ScalarFunc::Coalesce:
                return "COALESCE";
            case ScalarFunc::Nullif:
                return "NULLIF";
            case ScalarFunc::Upper:
                return "UPPER";
            case ScalarFunc::Lower:
                return "LOWER";
            case ScalarFunc::Length:
                return "LENGTH";
            case ScalarFunc::Trim:
                return "TRIM";
            case ScalarFunc::Substr:
                return "SUBSTR";
            case ScalarFunc::Abs:
                return "ABS";
            default:
                return "UNKNOWN";
        }
    }

    /** @brief 标量函数表达式，如 COALESCE、UPPER、SUBSTR 等。 */
    struct FunctionExpr {
        ScalarFunc func{ ScalarFunc::Coalesce }; ///< 函数类型
        std::vector<Expression> args;            ///< 参数列表

        /** @brief 获取函数的字符串表示。 */
        [[nodiscard]] std::string to_string() const {
            std::string result = scalar_func_name(func);
            result += "(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0)
                    result += ", ";
                result += "..."; // 简化表示
            }
            result += ")";
            return result;
        }
    };

    /** @brief 比较运算符。 */
    enum class CompareOp {
        Eq,       ///< 等于 (=)
        Ne,       ///< 不等于 (<> 或 !=)
        Lt,       ///< 小于 (<)
        Le,       ///< 小于等于 (<=)
        Gt,       ///< 大于 (>)
        Ge,       ///< 大于等于 (>=)
        Like,     ///< LIKE 模糊匹配
        NotLike,  ///< NOT LIKE
        IsNull,   ///< IS NULL
        IsNotNull ///< IS NOT NULL
    };

    /** @brief 将比较运算符转为字符串。 */
    [[nodiscard]] inline const char* compare_op_str(CompareOp op) {
        switch (op) {
            case CompareOp::Eq:
                return "=";
            case CompareOp::Ne:
                return "<>";
            case CompareOp::Lt:
                return "<";
            case CompareOp::Le:
                return "<=";
            case CompareOp::Gt:
                return ">";
            case CompareOp::Ge:
                return ">=";
            case CompareOp::Like:
                return "LIKE";
            case CompareOp::NotLike:
                return "NOT LIKE";
            case CompareOp::IsNull:
                return "IS NULL";
            case CompareOp::IsNotNull:
                return "IS NOT NULL";
            default:
                return "?";
        }
    }

    /** @brief SQL 比较表达式（如 column = value、a > b）。 */
    struct Comparison {
        Expression lhs;                ///< 左操作数
        CompareOp op{ CompareOp::Eq }; ///< 比较运算符
        Expression rhs;                ///< 右操作数
    };

    /** @brief SQL IN / NOT IN 表达式。 */
    struct InExpr {
        Expression expr;                ///< 左侧表达式
        std::vector<Expression> values; ///< 值列表
        bool negated{ false };          ///< 是否为 NOT IN
    };

    /** @brief SQL BETWEEN / NOT BETWEEN 表达式。 */
    struct BetweenExpr {
        Expression expr;       ///< 测试表达式
        Expression low;        ///< 下界
        Expression high;       ///< 上界
        bool negated{ false }; ///< 是否为 NOT BETWEEN
    };

    /** @brief 布尔表达式，支持比较、AND、OR、NOT、IN、BETWEEN。 */
    struct BoolExpr {
        /** @brief 布尔表达式类型。 */
        enum class Kind {
            Comparison, ///< 比较表达式
            And,        ///< 逻辑与
            Or,         ///< 逻辑或
            Not,        ///< 逻辑非
            In,         ///< IN 表达式
            Between     ///< BETWEEN 表达式
        };

        Kind kind{ Kind::Comparison };           ///< 表达式类型
        std::unique_ptr<BoolExpr> left;          ///< 左子表达式（用于AND、OR、NOT）
        std::unique_ptr<BoolExpr> right;         ///< 右子表达式（用于AND、OR）
        std::optional<Comparison> cmp;           ///< 比较表达式（当kind为Comparison时有效）
        std::optional<InExpr> in_expr;           ///< IN表达式（当kind为In时有效）
        std::optional<BetweenExpr> between_expr; ///< BETWEEN表达式（当kind为Between时有效）

        /// 默认构造函数
        BoolExpr() = default;

        /** @brief 深复制构造函数（unique_ptr 需要自定义实现）。 */
        BoolExpr(const BoolExpr& other) {
            *this = other;
        }

        /** @brief 深复制赋值运算符。 */
        BoolExpr& operator=(const BoolExpr& other) {
            if (this == &other)
                return *this;
            kind = other.kind;
            cmp = other.cmp;
            in_expr = other.in_expr;
            between_expr = other.between_expr;
            left = other.left ? std::make_unique<BoolExpr>(*other.left) : nullptr;
            right = other.right ? std::make_unique<BoolExpr>(*other.right) : nullptr;
            return *this;
        }

        /// 移动构造函数
        BoolExpr(BoolExpr&&) noexcept = default;
        /// 移动赋值运算符
        BoolExpr& operator=(BoolExpr&&) noexcept = default;

        /** @brief 检查是否为叶子节点（比较表达式）。 */
        [[nodiscard]] bool is_leaf() const noexcept {
            return kind == Kind::Comparison;
        }

        /** @brief 检查是否为 AND 表达式。 */
        [[nodiscard]] bool is_and() const noexcept {
            return kind == Kind::And;
        }

        /** @brief 检查是否为 OR 表达式。 */
        [[nodiscard]] bool is_or() const noexcept {
            return kind == Kind::Or;
        }

        /** @brief 检查是否为 NOT 表达式。 */
        [[nodiscard]] bool is_not() const noexcept {
            return kind == Kind::Not;
        }

        /** @brief 检查是否为 IN 表达式。 */
        [[nodiscard]] bool is_in() const noexcept {
            return kind == Kind::In;
        }

        /** @brief 检查是否为 BETWEEN 表达式。 */
        [[nodiscard]] bool is_between() const noexcept {
            return kind == Kind::Between;
        }

        /** @brief 创建比较表达式。 */
        static BoolExpr make_comparison(Comparison comparison) {
            BoolExpr expr;
            expr.kind = Kind::Comparison;
            expr.cmp = std::move(comparison);
            return expr;
        }

        /** @brief 创建 AND 表达式。 */
        static BoolExpr make_and(BoolExpr lhs, BoolExpr rhs) {
            BoolExpr expr;
            expr.kind = Kind::And;
            expr.left = std::make_unique<BoolExpr>(std::move(lhs));
            expr.right = std::make_unique<BoolExpr>(std::move(rhs));
            return expr;
        }

        /** @brief 创建 OR 表达式。 */
        static BoolExpr make_or(BoolExpr lhs, BoolExpr rhs) {
            BoolExpr expr;
            expr.kind = Kind::Or;
            expr.left = std::make_unique<BoolExpr>(std::move(lhs));
            expr.right = std::make_unique<BoolExpr>(std::move(rhs));
            return expr;
        }

        /** @brief 创建 NOT 表达式。 */
        static BoolExpr make_not(BoolExpr operand) {
            BoolExpr expr;
            expr.kind = Kind::Not;
            expr.left = std::make_unique<BoolExpr>(std::move(operand));
            return expr;
        }

        /** @brief 创建 IN 表达式。 */
        static BoolExpr make_in(InExpr in_expression) {
            BoolExpr expr;
            expr.kind = Kind::In;
            expr.in_expr = std::move(in_expression);
            return expr;
        }

        /** @brief 创建 BETWEEN 表达式。 */
        static BoolExpr make_between(BetweenExpr between_expression) {
            BoolExpr expr;
            expr.kind = Kind::Between;
            expr.between_expr = std::move(between_expression);
            return expr;
        }
    };

    /** @brief SELECT 语句 AST 节点。 */
    struct SelectStmt {
        /** @brief SELECT 子句中的一个输出项（表达式或聚合函数）。 */
        struct SelectItem {
            std::variant<Expression, AggregateExpr> value; ///< 输出表达式
            std::optional<std::string> alias;              ///< 别名（AS子句）

            /** @brief 检查是否为聚合表达式。 */
            [[nodiscard]] bool is_aggregate() const noexcept {
                return std::holds_alternative<AggregateExpr>(value);
            }

            /** @brief 检查是否有别名（AS 子句）。 */
            [[nodiscard]] bool has_alias() const noexcept {
                return alias.has_value();
            }
        };

        /** @brief FROM 子句中的一个连接项。 */
        struct Join {
            JoinType type{ JoinType::Inner }; ///< 连接类型
            std::string table;                ///< 要连接的表名
            std::optional<BoolExpr> on;       ///< ON条件
            std::optional<std::string> alias; ///< 表别名
        };

        std::vector<SelectItem> projections;   ///< SELECT投影列表
        bool distinct{ false };                ///< DISTINCT 去重标志
        std::string from_table;                ///< 主表名（FROM子句）
        std::optional<std::string> from_alias; ///< 主表别名
        std::vector<Join> joins;               ///< JOIN子句列表
        std::optional<BoolExpr> where;         ///< WHERE条件

        /** @brief ORDER BY 子句中的一个排序项。 */
        struct OrderByItem {
            std::variant<Expression, AggregateExpr> key; ///< 排序键
            bool asc{ true };                            ///< 是否升序

            /** @brief 检查是否为升序。 */
            [[nodiscard]] bool is_ascending() const noexcept {
                return asc;
            }

            /** @brief 检查是否为降序。 */
            [[nodiscard]] bool is_descending() const noexcept {
                return !asc;
            }
        };

        std::vector<OrderByItem> order_by; ///< ORDER BY子句
        std::vector<ColumnRef> group_by;   ///< GROUP BY子句
        std::optional<BoolExpr> having;    ///< HAVING条件
        std::optional<int64_t> limit;      ///< LIMIT值
        std::optional<int64_t> offset;     ///< OFFSET值

        /** @brief 检查是否有 WHERE 子句。 */
        [[nodiscard]] bool has_where() const noexcept {
            return where.has_value();
        }

        /** @brief 检查是否有 ORDER BY 子句。 */
        [[nodiscard]] bool has_order_by() const noexcept {
            return !order_by.empty();
        }

        /** @brief 检查是否有 GROUP BY 子句。 */
        [[nodiscard]] bool has_group_by() const noexcept {
            return !group_by.empty();
        }

        /** @brief 检查是否有 LIMIT 子句。 */
        [[nodiscard]] bool has_limit() const noexcept {
            return limit.has_value();
        }

        /** @brief 检查是否有 JOIN 子句。 */
        [[nodiscard]] bool has_joins() const noexcept {
            return !joins.empty();
        }

        /** @brief 检查是否有 DISTINCT。 */
        [[nodiscard]] bool has_distinct() const noexcept {
            return distinct;
        }
    };

    /** @brief INSERT 语句 AST 节点。 */
    struct InsertStmt {
        std::string table;                         ///< 目标表名
        std::vector<std::string> columns;          ///< 目标列列表（空表示所有列）
        std::vector<std::vector<Expression>> rows; ///< 插入的多行值列表

        /** @brief 检查是否显式指定了列列表。 */
        [[nodiscard]] bool has_column_list() const noexcept {
            return !columns.empty();
        }

        /** @brief 获取插入的行数。 */
        [[nodiscard]] std::size_t row_count() const noexcept {
            return rows.size();
        }
    };

    /** @brief CREATE TABLE 中的列定义。 */
    struct ColumnDef {
        std::string name;                        ///< 列名
        TypeKind type{ TypeKind::Int64 };        ///< 数据类型
        bool primary_key{ false };               ///< PRIMARY KEY 约束
        bool not_null{ false };                  ///< NOT NULL 约束
        std::optional<Expression> default_value; ///< DEFAULT 值

        /** @brief 检查是否为整数类型。 */
        [[nodiscard]] bool is_integer() const noexcept {
            return type == TypeKind::Int64;
        }

        /** @brief 检查是否为字符串类型。 */
        [[nodiscard]] bool is_string() const noexcept {
            return type == TypeKind::Text;
        }

        /** @brief 检查是否有列约束。 */
        [[nodiscard]] bool has_constraints() const noexcept {
            return primary_key || not_null || default_value.has_value();
        }
    };

    /** @brief CREATE TABLE 语句 AST 节点。 */
    struct CreateStmt {
        std::string table;              ///< 新表名称
        std::vector<ColumnDef> columns; ///< 列定义列表
    };

    /** @brief CREATE INDEX 语句 AST 节点。 */
    struct CreateIndexStmt {
        std::string index_name; ///< 索引名称
        std::string table;      ///< 目标表名
        std::string column;     ///< 索引列名
    };

    /** @brief UPDATE SET 子句中的一个赋值操作。 */
    struct UpdateAssign {
        std::string column; ///< 目标列名
        Expression value;   ///< 新值表达式
    };

    /** @brief UPDATE 语句 AST 节点。 */
    struct UpdateStmt {
        std::string table;                     ///< 目标表名
        std::vector<UpdateAssign> assignments; ///< 赋值列表
        std::optional<BoolExpr> where;         ///< WHERE条件

        /** @brief 检查是否有 WHERE 条件。 */
        [[nodiscard]] bool has_where() const noexcept {
            return where.has_value();
        }
    };

    /** @brief DELETE 语句 AST 节点。 */
    struct DeleteStmt {
        std::string table;             ///< 目标表名
        std::optional<BoolExpr> where; ///< WHERE条件

        /** @brief 检查是否有 WHERE 条件。 */
        [[nodiscard]] bool has_where() const noexcept {
            return where.has_value();
        }
    };

    /** @brief DROP TABLE 语句 AST 节点。 */
    struct DropTableStmt {
        std::string table;       ///< 要删除的表名
        bool if_exists{ false }; ///< IF EXISTS 标志
    };

    /** @brief DROP INDEX 语句 AST 节点。 */
    struct DropIndexStmt {
        std::string index_name;  ///< 要删除的索引名
        std::string table;       ///< 索引所在的表名
        bool if_exists{ false }; ///< IF EXISTS 标志
    };

    /**
     * @struct BeginStmt
     * @brief BEGIN TRANSACTION语句结构
     *
     * 表示SQL BEGIN/BEGIN TRANSACTION语句，用于开始一个新事务。
     *
     * @par SQL语法：
     * ```sql
     * BEGIN [TRANSACTION]
     * ```
     */
    struct BeginStmt {
        // 目前不需要额外字段，未来可扩展为支持事务隔离级别
    };

    /**
     * @struct CommitStmt
     * @brief COMMIT语句结构
     *
     * 表示SQL COMMIT/COMMIT TRANSACTION语句，用于提交当前事务。
     *
     * @par SQL语法：
     * ```sql
     * COMMIT [TRANSACTION]
     * ```
     */
    struct CommitStmt {
        // 目前不需要额外字段
    };

    /**
     * @struct RollbackStmt
     * @brief ROLLBACK语句结构
     *
     * 表示SQL ROLLBACK/ROLLBACK TRANSACTION语句，用于回滚当前事务。
     *
     * @par SQL语法：
     * ```sql
     * ROLLBACK [TRANSACTION]
     * ```
     */
    struct RollbackStmt {
        // 目前不需要额外字段
    };

    /** @struct CheckpointStmt @brief CHECKPOINT 语句，强制刷盘并截断 WAL。 */
    struct CheckpointStmt {};

    /** @struct ShowStatusStmt @brief SHOW STATUS 语句，显示服务器运行指标。 */
    struct ShowStatusStmt {};

    /** @struct PrepareStmt @brief PREPARE 语句，缓存执行计划。 */
    struct PrepareStmt {
        std::string name;    ///< 预处理语句名称
        std::string sql;     ///< 要准备的 SQL 文本
    };

    /** @struct ExecuteStmt @brief EXECUTE 语句，执行已缓存的预处理语句。 */
    struct ExecuteStmt {
        std::string name;    ///< 预处理语句名称
    };

    /** @struct DeallocateStmt @brief DEALLOCATE PREPARE 语句，清理预处理语句。 */
    struct DeallocateStmt {
        std::string name;    ///< 预处理语句名称（空 = 全部）
    };

    /** @struct AuthStmt @brief AUTH 语句，客户端认证。 */
    struct AuthStmt {
        std::string username;
        std::string password;
    };

    /** @struct CreateUserStmt @brief CREATE USER 语句，创建数据库用户。 */
    struct CreateUserStmt {
        std::string username;
        std::string password;
    };

    /**
     * @struct SetTransactionStmt
     * @brief SET TRANSACTION ISOLATION LEVEL 语句
     *
     * 设置当前会话的事务隔离级别。仅在不在事务中时（自动提交模式）生效，
     * 设置后下一个 BEGIN 开启的事务沿用此隔离级别。
     *
     * @par SQL语法：
     * ```sql
     * SET TRANSACTION ISOLATION LEVEL
     *   { READ UNCOMMITTED | READ COMMITTED | REPEATABLE READ | SERIALIZABLE }
     * ```
     */
    struct SetTransactionStmt {
        /// 0=ReadUncommitted 1=ReadCommitted 2=RepeatableRead 3=Serializable
        /// 用整数避免 ast.h 反向依赖 session.h
        int isolation_level{ 1 };
    };

    /** @brief SQL 语句（所有支持语句类型的 variant）。 */
    using Statement = std::variant<SelectStmt, InsertStmt, UpdateStmt, DeleteStmt, CreateStmt, CreateIndexStmt,
                                   DropTableStmt, DropIndexStmt, BeginStmt, CommitStmt, RollbackStmt,
                                   CheckpointStmt, ShowStatusStmt,
                                   PrepareStmt, ExecuteStmt, DeallocateStmt, AuthStmt, CreateUserStmt,
                                   SetTransactionStmt, std::shared_ptr<ExplainStmt>>;

    /** @brief EXPLAIN 语句 AST 节点。 */
    struct ExplainStmt {
        Statement inner;    ///< 要解释的内部SQL语句
        bool analyze{ false }; ///< 是否执行并收集运行时统计
    };

} // namespace corodb
