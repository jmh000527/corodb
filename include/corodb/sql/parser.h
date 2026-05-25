// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file parser.h @brief SQL 解析器定义。 */

#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "corodb/ast/ast.h"

namespace corodb {

    /** @brief 返回字符串的大写副本。 */
    [[nodiscard]] std::string to_upper(std::string s);

    /** @brief 将 SQL 文本转换为 AST。 */
    class Parser {
    public:
        Parser() = default;

        /** @brief 解析一条 SQL 语句。 */
        Statement parse(const std::string& sql);

        [[nodiscard]] bool at_end() const noexcept {
            return pos_ >= tokens_.size();
        }

        [[nodiscard]] std::size_t position() const noexcept {
            return pos_;
        }

    private:
        /** @brief 词法单元。 */
        struct Token {
            std::string text;

            [[nodiscard]] bool empty() const noexcept {
                return text.empty();
            }

            [[nodiscard]] std::string upper() const {
                return to_upper(text);
            }
        };

        /// 解析 SELECT 语句。
        [[nodiscard]] Statement parse_select();

        /// 解析 EXPLAIN 语句。
        [[nodiscard]] Statement parse_explain();

        /// 解析 CREATE 语句，分发到具体的 CREATE 类型。
        [[nodiscard]] Statement parse_create();

        /// 解析 CREATE TABLE 语句。
        [[nodiscard]] Statement parse_create_table();

        /// 解析 CREATE INDEX 语句。
        [[nodiscard]] Statement parse_create_index();

        /// 解析 DROP 语句，分发到具体的 DROP 类型。
        [[nodiscard]] Statement parse_drop();

        /// 解析 INSERT 语句。
        [[nodiscard]] Statement parse_insert();

        /// 解析 UPDATE 语句。
        [[nodiscard]] Statement parse_update();

        /// 解析 DELETE 语句。
        [[nodiscard]] Statement parse_delete();

        /// 解析 BEGIN 语句。
        [[nodiscard]] BeginStmt parse_begin();

        /// 解析 COMMIT 语句。
        [[nodiscard]] CommitStmt parse_commit();

        /// 解析 ROLLBACK 语句。
        [[nodiscard]] RollbackStmt parse_rollback();
        /// 解析 CHECKPOINT 语句。
        [[nodiscard]] CheckpointStmt parse_checkpoint();
        /// 解析 SHOW STATUS 语句。
        [[nodiscard]] ShowStatusStmt parse_show();
        /// 解析 PREPARE 语句。
        [[nodiscard]] PrepareStmt parse_prepare();
        /// 解析 EXECUTE 语句。
        [[nodiscard]] ExecuteStmt parse_execute();
        /// 解析 DEALLOCATE PREPARE 语句。
        [[nodiscard]] DeallocateStmt parse_deallocate();
        /// 解析 AUTH 语句。
        [[nodiscard]] AuthStmt parse_auth();
        /// 解析 CREATE USER 语句。
        [[nodiscard]] CreateUserStmt parse_create_user();

        /// 解析 SET TRANSACTION ISOLATION LEVEL 语句。
        [[nodiscard]] SetTransactionStmt parse_set();

        /// 解析 SELECT 投影项列表。
        [[nodiscard]] std::vector<SelectStmt::SelectItem> parse_projections();

        /// 解析单个 SELECT 投影项。
        [[nodiscard]] SelectStmt::SelectItem parse_select_item();

        /// 解析 ORDER BY 子句。
        [[nodiscard]] std::vector<SelectStmt::OrderByItem> parse_order_by();

        /// 解析 GROUP BY 子句。
        [[nodiscard]] std::vector<ColumnRef> parse_group_by();

        /// 解析布尔表达式（入口点）。
        [[nodiscard]] BoolExpr parse_bool_expr();

        /// 解析 OR 表达式。
        [[nodiscard]] BoolExpr parse_or();

        /// 解析 AND 表达式。
        [[nodiscard]] BoolExpr parse_and();

        /// 解析 NOT 表达式。
        [[nodiscard]] BoolExpr parse_not();

        /// 解析布尔原子表达式（括号或比较）。
        [[nodiscard]] BoolExpr parse_bool_atom();

        /// 解析比较表达式。
        [[nodiscard]] BoolExpr parse_comparison();

        /// 解析算术表达式（入口点）。
        [[nodiscard]] Expression parse_expression();

        /// 解析加减表达式。
        [[nodiscard]] Expression parse_additive();

        /// 解析乘除表达式。
        [[nodiscard]] Expression parse_term();

        /// 解析一元/原子表达式。
        [[nodiscard]] Expression parse_factor();

        /// 解析字面量、列引用或函数调用表达式。
        [[nodiscard]] Expression parse_literal_expression();

        /// 解析 UPDATE SET 子句中的赋值操作。
        [[nodiscard]] UpdateAssign parse_assignment();

        /// 解析聚合函数表达式。
        [[nodiscard]] AggregateExpr parse_aggregate();

        /// 解析标量函数表达式。
        [[nodiscard]] Expression parse_scalar_function();

        /// 解析列引用（可能包含表名限定）。
        [[nodiscard]] ColumnRef parse_column_ref();

        /// 解析字面量值。
        [[nodiscard]] Value parse_literal();

        /// 解析整数字面量。
        [[nodiscard]] int64_t parse_int_literal();

        /// 将 SQL 文本切分为 Token 序列。
        [[nodiscard]] std::vector<Token> tokenize(const std::string& sql);

        /// 消费当前 Token 并前进，若已到末尾则抛出异常。
        const Token& consume();

        /// 消费标识符 Token，若非标识符则抛出异常。
        [[nodiscard]] std::string consume_identifier();

        /// 消费操作符 Token。
        [[nodiscard]] std::string consume_operator();

        // ==================== Token匹配方法 ====================

        /// 若当前 Token 文本匹配则消费并返回 true。
        [[nodiscard]] bool match_text(std::string_view text);

        /// 若当前 Token 关键字匹配（不区分大小写）则消费并返回 true。
        bool match_keyword(std::string_view kw);

        /// 期望并消费指定关键字，不匹配则抛出异常。
        void expect_keyword(std::string_view kw);

        /// 期望并消费指定文本，不匹配则抛出异常。
        void expect_text(std::string_view text);

        /// 检查当前 Token 是否为标识符。
        [[nodiscard]] bool peek_is_identifier() const;

        /// 检查指定文本是否为有效标识符。
        [[nodiscard]] static bool peek_is_identifier_text(const std::string& text);

        /// 返回当前 Token 的大写形式。
        [[nodiscard]] std::string peek_upper() const;

        // ==================== 成员变量 ====================

        std::vector<Token> tokens_; ///< 词法分析后的Token列表
        std::size_t pos_{ 0 };      ///< 当前解析位置
    };

} // namespace corodb
