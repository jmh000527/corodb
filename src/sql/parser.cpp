// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file parser.cpp
// @brief SQL 解析器（词法+语法分析）的实现。

#include "corodb/sql/parser.h"

#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace corodb {

    namespace {
        /**
         * @brief SQL保留关键字集合（用于SELECT语句上下文）
         *
         * 使用静态unordered_set进行O(1)查找，替代多次字符串比较
         */
        const std::unordered_set<std::string> kSelectReservedKeywords = { "WHERE", "GROUP", "ORDER", "LIMIT", "OFFSET",
                                                                          "JOIN",  "LEFT",  "RIGHT", "FULL",  "OUTER",
                                                                          "ON",    "INNER", "HAVING", "UNION" };

        /**
         * @brief SQL保留关键字集合（用于投影项上下文）
         */
        const std::unordered_set<std::string> kProjectionReservedKeywords = { "FROM",  "WHERE",  "GROUP", "ORDER",
                                                                              "LIMIT", "OFFSET", "JOIN",  "LEFT",
                                                                              "RIGHT", "FULL",   "OUTER", "INNER" };

        /**
         * @brief 检查标识符是否为SELECT上下文的保留关键字
         * @param word 要检查的大写标识符
         * @return true 如果是保留关键字
         */
        inline bool is_select_reserved(const std::string& word) {
            return kSelectReservedKeywords.contains(word);
        }

        /**
         * @brief 检查标识符是否为投影项上下文的保留关键字
         * @param word 要检查的大写标识符
         * @return true 如果是保留关键字
         */
        inline bool is_projection_reserved(const std::string& word) {
            return kProjectionReservedKeywords.contains(word);
        }
    } // namespace

    /**
     * @brief 将字符串转换为大写
     * @param s 输入字符串
     * @return 转换后的大写字符串
     */
    std::string to_upper(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    /**
     * @brief 解析SQL语句的入口函数
     * @param sql 要解析的SQL字符串
     * @return 解析后的语句对象，类型为Statement
     */
    Statement Parser::parse(const std::string& sql) {
        tokens_ = tokenize(sql);  // 将SQL字符串分词
        pos_ = 0;                 // 重置解析位置
        auto head = peek_upper(); // 查看第一个token的大写形式
        if (head == "EXPLAIN")
            return parse_explain(); // 解析EXPLAIN语句
        if (head == "CREATE")
            return parse_create(); // 解析CREATE语句
        if (head == "DROP")
            return parse_drop(); // 解析DROP语句
        if (head == "SELECT")
            return parse_select(); // 解析SELECT语句
        if (head == "WITH")
            return parse_with_select(); // 解析 WITH ... SELECT（CTE 内联改写）
        if (head == "INSERT")
            return parse_insert(); // 解析INSERT语句
        if (head == "UPDATE")
            return parse_update(); // 解析UPDATE语句
        if (head == "DELETE")
            return parse_delete(); // 解析DELETE语句
        if (head == "BEGIN")
            return parse_begin(); // 解析BEGIN语句
        if (head == "COMMIT")
            return parse_commit(); // 解析COMMIT语句
        if (head == "ROLLBACK")
            return parse_rollback(); // 解析ROLLBACK语句（含 ROLLBACK TO SAVEPOINT）
        if (head == "SAVEPOINT") {
            consume(); // 消费 SAVEPOINT
            return SavepointStmt{ consume_identifier() };
        }
        if (head == "RELEASE") {
            consume();                  // 消费 RELEASE
            match_keyword("SAVEPOINT"); // 可选的 SAVEPOINT 关键字
            return ReleaseSavepointStmt{ consume_identifier() };
        }
        if (head == "CHECKPOINT")
            return parse_checkpoint(); // 解析 CHECKPOINT
        if (head == "SHOW")
            return parse_show(); // 解析 SHOW STATUS
        if (head == "PREPARE")
            return parse_prepare(); // 解析 PREPARE
        if (head == "EXECUTE")
            return parse_execute(); // 解析 EXECUTE
        if (head == "DEALLOCATE")
            return parse_deallocate(); // 解析 DEALLOCATE PREPARE
        if (head == "AUTH")
            return parse_auth(); // 解析 AUTH
        if (head == "SET")
            return parse_set(); // 解析 SET TRANSACTION ...
        throw std::runtime_error("[Parser] Unsupported statement type: " + head);
    }

    /**
     * @brief 解析EXPLAIN语句
     * @return 解析后的EXPLAIN语句对象
     */
    Statement Parser::parse_explain() {
        expect_keyword("EXPLAIN");               // 期望EXPLAIN关键字
        bool analyze = match_keyword("ANALYZE"); // 检查是否为 EXPLAIN ANALYZE
        auto head = peek_upper();                // 查看下一个token
        Statement inner;
        if (head == "SELECT")
            inner = parse_select(); // 解析SELECT子句
        else if (head == "INSERT")
            inner = parse_insert(); // 解析INSERT子句
        else if (head == "UPDATE")
            inner = parse_update(); // 解析UPDATE子句
        else if (head == "DELETE")
            inner = parse_delete(); // 解析DELETE子句
        else
            throw std::runtime_error("[Parser] EXPLAIN requires a query statement");
        return std::make_shared<ExplainStmt>(ExplainStmt{ std::move(inner), analyze }); // 返回EXPLAIN语句对象
    }

    /**
     * @brief 解析SELECT语句
     * @return 解析后的SELECT语句对象
     */
    Statement Parser::parse_select() {
        Statement first = parse_select_core();
        auto& stmt = std::get<SelectStmt>(first);
        // UNION [ALL] 拼接：顶层循环收集各臂（臂用 core 解析，不吞后续 UNION，保持放平）。
        while (match_keyword("UNION")) {
            SelectStmt::UnionArm arm;
            arm.all = match_keyword("ALL");
            Statement next = parse_select_core();
            arm.select = std::make_shared<SelectStmt>(std::move(std::get<SelectStmt>(next)));
            stmt.unions.push_back(std::move(arm));
        }
        // v1 约束：各臂 UNION/UNION ALL 类型须一致（混合的左结合语义暂不支持）。
        for (std::size_t i = 1; i < stmt.unions.size(); ++i) {
            if (stmt.unions[i].all != stmt.unions[0].all)
                throw std::runtime_error("[Parser] Mixed UNION and UNION ALL is not supported");
        }
        return first;
    }

    namespace {

        // 将表达式中限定为 from 表名的列引用改限定为 to（CTE 内联后基表被别名遮盖）。
        void requalify_expr(Expression& e, const std::string& from, const std::string& to) {
            if (auto* ref = std::get_if<ColumnRef>(&e)) {
                if (ref->table == from)
                    ref->table = to;
                return;
            }
            if (auto* bin = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
                if (*bin) {
                    requalify_expr((*bin)->lhs, from, to);
                    requalify_expr((*bin)->rhs, from, to);
                }
                return;
            }
            if (auto* fn = std::get_if<std::shared_ptr<FunctionExpr>>(&e)) {
                if (*fn)
                    for (auto& a: (*fn)->args)
                        requalify_expr(a, from, to);
            }
        }

        void requalify_bool(BoolExpr& b, const std::string& from, const std::string& to) {
            if (b.left)
                requalify_bool(*b.left, from, to);
            if (b.right)
                requalify_bool(*b.right, from, to);
            if (b.cmp.has_value()) {
                requalify_expr(b.cmp->lhs, from, to);
                requalify_expr(b.cmp->rhs, from, to);
            }
            if (b.in_expr.has_value()) {
                requalify_expr(b.in_expr->expr, from, to);
                for (auto& v: b.in_expr->values)
                    requalify_expr(v, from, to);
            }
            if (b.between_expr.has_value()) {
                requalify_expr(b.between_expr->expr, from, to);
                requalify_expr(b.between_expr->low, from, to);
                requalify_expr(b.between_expr->high, from, to);
            }
        }

        /// CTE 定义（v1：body 限 SELECT * FROM 单表 [WHERE ...]）。
        struct CteDef {
            std::string base_table;
            std::optional<BoolExpr> where;
        };

    } // namespace

    /**
     * @brief 解析 WITH name AS (SELECT ...) [, ...] SELECT ...（CTE v1，解析期内联）。
     *
     * body 限制为 `SELECT * FROM 单表 [WHERE ...]`；主查询（含 UNION 臂）中对 CTE 名的引用
     * 改写为基表 + 别名（无显式别名时别名 = CTE 名），body WHERE 合取并入引用处
     *（FROM 位置并入主 WHERE；INNER/LEFT JOIN 并入 ON；RIGHT/FULL + body WHERE 拒绝，
     * ON 合并与输入预过滤在该两类外连接下不等价）。
     */
    Statement Parser::parse_with_select() {
        expect_keyword("WITH");
        std::unordered_map<std::string, CteDef> ctes;
        while (true) {
            std::string name = consume_identifier();
            expect_keyword("AS");
            expect_text("(");
            Statement body_stmt = parse_select();
            expect_text(")");
            auto& body = std::get<SelectStmt>(body_stmt);
            const bool star_only = body.projections.size() == 1 &&
                                   std::holds_alternative<Expression>(body.projections[0].value) &&
                                   std::holds_alternative<ColumnRef>(
                                           std::get<Expression>(body.projections[0].value)) &&
                                   std::get<ColumnRef>(std::get<Expression>(body.projections[0].value)).name == "*";
            if (!star_only || !body.joins.empty() || body.distinct || !body.group_by.empty() ||
                body.having.has_value() || !body.order_by.empty() || body.limit.has_value() ||
                body.offset.has_value() || !body.unions.empty() || body.from_alias.has_value()) {
                throw std::runtime_error("[Parser] CTE body must be 'SELECT * FROM table [WHERE ...]' (v1)");
            }
            CteDef def;
            def.base_table = body.from_table;
            def.where = std::move(body.where);
            if (!ctes.emplace(std::move(name), std::move(def)).second)
                throw std::runtime_error("[Parser] Duplicate CTE name");
            if (!match_text(","))
                break;
        }
        Statement main_stmt = parse_select();
        auto& main_sel = std::get<SelectStmt>(main_stmt);

        // 对单个 SelectStmt 内联改写 CTE 引用。
        auto inline_into = [&](SelectStmt& s) {
            if (auto it = ctes.find(s.from_table); it != ctes.end()) {
                const std::string alias = s.from_alias.value_or(s.from_table);
                s.from_table = it->second.base_table;
                s.from_alias = alias;
                if (it->second.where.has_value()) {
                    BoolExpr merged = *it->second.where; // 深拷贝
                    requalify_bool(merged, it->second.base_table, alias);
                    s.where = s.where.has_value() ? BoolExpr::make_and(std::move(merged), std::move(*s.where))
                                                  : std::optional<BoolExpr>(std::move(merged));
                }
            }
            for (auto& j: s.joins) {
                auto jit = ctes.find(j.table);
                if (jit == ctes.end())
                    continue;
                if (jit->second.where.has_value() && (j.type == JoinType::Right || j.type == JoinType::Full))
                    throw std::runtime_error(
                            "[Parser] CTE with WHERE is not supported on the RIGHT/FULL JOIN side (v1)");
                const std::string alias = j.alias.value_or(j.table);
                j.table = jit->second.base_table;
                j.alias = alias;
                if (jit->second.where.has_value()) {
                    BoolExpr merged = *jit->second.where;
                    requalify_bool(merged, jit->second.base_table, alias);
                    if (j.on.has_value())
                        j.on = BoolExpr::make_and(std::move(merged), std::move(*j.on));
                    else
                        j.on = std::move(merged);
                }
            }
        };
        inline_into(main_sel);
        for (auto& arm: main_sel.unions)
            if (arm.select)
                inline_into(*arm.select);
        return main_stmt;
    }

    Statement Parser::parse_select_core() {
        expect_keyword("SELECT"); // 期望SELECT关键字

        // 检查 DISTINCT 关键字
        bool distinct = match_keyword("DISTINCT");

        auto projections = parse_projections();  // 解析投影列
        expect_keyword("FROM");                  // 期望FROM关键字
        std::string base = consume_identifier(); // 解析表名

        std::optional<std::string> base_alias; // 表别名
        if (peek_is_identifier() && !is_select_reserved(peek_upper())) {
            base_alias = consume_identifier(); // 解析表别名
        }

        std::vector<SelectStmt::Join> joins; // JOIN子句列表
        while (true) {
            JoinType type = JoinType::Inner; // 默认内连接
            if (match_keyword("LEFT")) {
                type = JoinType::Left;  // 左外连接
                match_keyword("OUTER"); // 可选的OUTER关键字
                expect_keyword("JOIN"); // 期望JOIN关键字
            } else if (match_keyword("RIGHT")) {
                type = JoinType::Right; // 右外连接
                match_keyword("OUTER"); // 可选的OUTER关键字
                expect_keyword("JOIN"); // 期望JOIN关键字
            } else if (match_keyword("FULL")) {
                type = JoinType::Full;  // 全外连接
                match_keyword("OUTER"); // 可选的OUTER关键字
                expect_keyword("JOIN"); // 期望JOIN关键字
            } else if (match_keyword("INNER")) {
                type = JoinType::Inner; // 显式内连接
                expect_keyword("JOIN"); // 期望JOIN关键字
            } else if (match_keyword("JOIN")) {
                type = JoinType::Inner; // 内连接
            } else {
                break; // 没有更多JOIN子句
            }

            std::string tbl = consume_identifier(); // 解析连接表名
            std::optional<std::string> alias;       // 连接表别名
            if (peek_is_identifier() && !is_select_reserved(peek_upper())) {
                alias = consume_identifier(); // 解析连接表别名
            }
            std::optional<BoolExpr> on; // ON条件
            if (match_keyword("ON")) {
                on = parse_bool_expr(); // 解析ON条件
            } else {
                throw std::runtime_error("[Parser] JOIN without ON is not supported");
            }
            joins.push_back(
                    SelectStmt::Join{ type, std::move(tbl), std::move(on), std::move(alias) }); // 添加到JOIN列表
        }

        std::optional<BoolExpr> where; // WHERE条件
        if (match_keyword("WHERE")) {
            where = parse_bool_expr(); // 解析WHERE条件
        }

        std::vector<ColumnRef> group_by; // GROUP BY子句
        if (match_keyword("GROUP")) {
            expect_keyword("BY");        // 期望BY关键字
            group_by = parse_group_by(); // 解析GROUP BY列
        }

        std::optional<BoolExpr> having; // HAVING条件
        if (match_keyword("HAVING")) {
            having = parse_bool_expr(); // 解析HAVING条件
        }

        std::vector<SelectStmt::OrderByItem> order_by; // ORDER BY子句
        if (match_keyword("ORDER")) {
            expect_keyword("BY");        // 期望BY关键字
            order_by = parse_order_by(); // 解析ORDER BY项
        }

        std::optional<int64_t> limit;  // LIMIT子句
        std::optional<int64_t> offset; // OFFSET子句
        if (match_keyword("LIMIT")) {
            limit = parse_int_literal(); // 解析LIMIT值
        }
        if (match_keyword("OFFSET")) {
            offset = parse_int_literal(); // 解析OFFSET值
        }

        SelectStmt stmt;                           // 构造SELECT语句对象
        stmt.projections = std::move(projections); // 设置投影列
        stmt.distinct = distinct;                  // 设置DISTINCT标志
        stmt.from_table = std::move(base);         // 设置表名
        stmt.from_alias = std::move(base_alias);   // 设置表别名
        stmt.joins = std::move(joins);             // 设置JOIN子句
        stmt.where = std::move(where);             // 设置WHERE条件
        stmt.order_by = std::move(order_by);       // 设置ORDER BY子句
        stmt.group_by = std::move(group_by);       // 设置GROUP BY子句
        stmt.having = std::move(having);           // 设置HAVING条件
        stmt.limit = limit;                        // 设置LIMIT
        stmt.offset = offset;                      // 设置OFFSET
        return stmt;                               // 返回SELECT语句对象
    }

    /**
     * @brief 解析INSERT语句
     * @return 解析后的INSERT语句对象
     */
    Statement Parser::parse_insert() {
        expect_keyword("INSERT");                 // 期望INSERT关键字
        match_keyword("INTO");                    // 可选的INTO关键字
        std::string table = consume_identifier(); // 解析表名

        std::vector<std::string> columns;            // 列名列表
        if (match_text("(")) {                       // 如果有列名列表
            columns.push_back(consume_identifier()); // 解析第一个列名
            while (match_text(",")) {
                columns.push_back(consume_identifier()); // 解析后续列名
            }
            expect_text(")"); // 期望右括号
        }

        expect_keyword("VALUES");                  // 期望VALUES关键字
        std::vector<std::vector<Expression>> rows; // 多行值列表

        // 解析第一行
        expect_text("(");                                // 期望左括号
        std::vector<Expression> first_row;               // 第一行值
        first_row.push_back(parse_literal_expression()); // 解析第一个值
        while (match_text(",")) {
            first_row.push_back(parse_literal_expression()); // 解析后续值
        }
        expect_text(")"); // 期望右括号
        rows.push_back(std::move(first_row));

        // 解析后续行（可选）
        while (match_text(",")) {
            expect_text("(");                          // 期望左括号
            std::vector<Expression> row;               // 当前行值
            row.push_back(parse_literal_expression()); // 解析第一个值
            while (match_text(",")) {
                row.push_back(parse_literal_expression()); // 解析后续值
            }
            expect_text(")"); // 期望右括号
            rows.push_back(std::move(row));
        }

        InsertStmt stmt;                   // 构造INSERT语句对象
        stmt.table = std::move(table);     // 设置表名
        stmt.columns = std::move(columns); // 设置列名列表
        stmt.rows = std::move(rows);       // 设置多行值列表
        return stmt;                       // 返回INSERT语句对象
    }

    /**
     * @brief 解析UPDATE语句
     * @return 解析后的UPDATE语句对象
     */
    Statement Parser::parse_update() {
        expect_keyword("UPDATE");                 // 期望UPDATE关键字
        std::string table = consume_identifier(); // 解析表名
        expect_keyword("SET");                    // 期望SET关键字

        std::vector<UpdateAssign> assigns;     // 更新赋值列表
        assigns.push_back(parse_assignment()); // 解析第一个赋值
        while (match_text(",")) {
            assigns.push_back(parse_assignment()); // 解析后续赋值
        }

        std::optional<BoolExpr> where; // WHERE条件
        if (match_keyword("WHERE")) {
            where = parse_bool_expr(); // 解析WHERE条件
        }

        UpdateStmt stmt;                       // 构造UPDATE语句对象
        stmt.table = std::move(table);         // 设置表名
        stmt.assignments = std::move(assigns); // 设置赋值列表
        stmt.where = std::move(where);         // 设置WHERE条件
        return stmt;                           // 返回UPDATE语句对象
    }

    /**
     * @brief 解析DELETE语句
     * @return 解析后的DELETE语句对象
     */
    Statement Parser::parse_delete() {
        expect_keyword("DELETE");                 // 期望DELETE关键字
        match_keyword("FROM");                    // 可选的FROM关键字
        std::string table = consume_identifier(); // 解析表名

        std::optional<BoolExpr> where; // WHERE条件
        if (match_keyword("WHERE")) {
            where = parse_bool_expr(); // 解析WHERE条件
        }

        DeleteStmt stmt;               // 构造DELETE语句对象
        stmt.table = std::move(table); // 设置表名
        stmt.where = std::move(where); // 设置WHERE条件
        return stmt;                   // 返回DELETE语句对象
    }

    /**
     * @brief 解析CREATE语句
     * @return 解析后的CREATE语句对象
     */
    Statement Parser::parse_create() {
        expect_keyword("CREATE"); // 期望CREATE关键字
        auto kw = peek_upper();   // 查看下一个token
        if (kw == "TABLE")
            return parse_create_table(); // 解析CREATE TABLE语句
        if (kw == "INDEX")
            return parse_create_index(); // 解析CREATE INDEX语句
        if (kw == "USER")
            return parse_create_user(); // 解析CREATE USER语句
        throw std::runtime_error("[Parser] Unsupported CREATE type: " + kw);
    }

    /**
     * @brief 解析CREATE TABLE语句
     * @return 解析后的CREATE TABLE语句对象
     */
    Statement Parser::parse_create_table() {
        expect_keyword("TABLE");                  // 期望TABLE关键字
        std::string table = consume_identifier(); // 解析表名
        expect_text("(");                         // 期望左括号

        std::vector<ColumnDef> cols; // 列定义列表
        if (!match_text(")")) {      // 如果表定义不为空
            while (true) {
                std::string col_name = consume_identifier();          // 解析列名
                std::string type_kw = to_upper(consume_identifier()); // 解析类型关键字
                TypeKind type;                                        // 列类型
                if (type_kw == "INT" || type_kw == "INT64" || type_kw == "BIGINT")
                    type = TypeKind::Int64;
                else if (type_kw == "BOOL" || type_kw == "BOOLEAN")
                    type = TypeKind::Boolean;
                else if (type_kw == "TEXT" || type_kw == "STRING" || type_kw == "VARCHAR")
                    type = TypeKind::Text;
                else if (type_kw == "DATE" || type_kw == "TIMESTAMP" || type_kw == "DATETIME")
                    type = TypeKind::Date;
                else if (type_kw == "FLOAT" || type_kw == "DOUBLE" || type_kw == "FLOAT64")
                    type = TypeKind::Float64;
                else if (type_kw == "DECIMAL" || type_kw == "NUMERIC")
                    type = TypeKind::Decimal;
                else
                    throw std::runtime_error("[Parser] Unsupported column type: " + type_kw);

                // 初始化列定义
                ColumnDef col_def;
                col_def.name = std::move(col_name);
                col_def.type = type;
                col_def.primary_key = false;
                col_def.not_null = false;
                col_def.default_value = std::nullopt;

                // 解析列约束（可选，可多个）
                while (true) {
                    auto kw = peek_upper();
                    if (kw == "PRIMARY") {
                        expect_keyword("PRIMARY");
                        expect_keyword("KEY");
                        col_def.primary_key = true;
                        col_def.not_null = true; // PRIMARY KEY 隐含 NOT NULL
                    } else if (kw == "NOT") {
                        expect_keyword("NOT");
                        expect_keyword("NULL");
                        col_def.not_null = true;
                    } else if (kw == "DEFAULT") {
                        expect_keyword("DEFAULT");
                        col_def.default_value = Literal{ parse_literal() };
                    } else {
                        break; // 没有更多约束
                    }
                }

                cols.push_back(std::move(col_def)); // 添加到列定义列表
                if (match_text(","))
                    continue;     // 下一个列定义
                expect_text(")"); // 期望右括号
                break;
            }
        }

        CreateStmt stmt;                // 构造CREATE语句对象
        stmt.table = std::move(table);  // 设置表名
        stmt.columns = std::move(cols); // 设置列定义列表
        return stmt;                    // 返回CREATE TABLE语句对象
    }

    /**
     * @brief 解析CREATE INDEX语句
     * @return 解析后的CREATE INDEX语句对象
     */
    Statement Parser::parse_create_index() {
        expect_keyword("INDEX");                   // 期望INDEX关键字
        std::string index = consume_identifier();  // 解析索引名
        expect_keyword("ON");                      // 期望ON关键字
        std::string table = consume_identifier();  // 解析表名
        expect_text("(");                          // 期望左括号
        std::vector<std::string> columns;
        columns.push_back(consume_identifier()); // 第一个索引列
        while (match_text(","))
            columns.push_back(consume_identifier()); // 额外的复合列
        expect_text(")");                          // 期望右括号

        CreateIndexStmt stmt;               // 构造CREATE INDEX语句对象
        stmt.index_name = std::move(index); // 设置索引名
        stmt.table = std::move(table);      // 设置表名
        stmt.columns = std::move(columns);  // 设置索引列（单/多列）
        return stmt;                        // 返回CREATE INDEX语句对象
    }

    /**
     * @brief 解析DROP语句
     * @return 解析后的DROP语句对象
     */
    Statement Parser::parse_drop() {
        expect_keyword("DROP"); // 期望DROP关键字
        auto kw = peek_upper(); // 查看下一个token
        if (kw == "TABLE") {
            expect_keyword("TABLE");
            bool if_exists = false;
            if (match_keyword("IF")) {
                expect_keyword("EXISTS");
                if_exists = true;
            }
            std::string table = consume_identifier();
            DropTableStmt stmt;
            stmt.table = std::move(table);
            stmt.if_exists = if_exists;
            return stmt;
        }
        if (kw == "INDEX") {
            expect_keyword("INDEX");
            bool if_exists = false;
            if (match_keyword("IF")) {
                expect_keyword("EXISTS");
                if_exists = true;
            }
            std::string index = consume_identifier();
            std::string table;
            if (match_keyword("ON")) {
                table = consume_identifier();
            }
            DropIndexStmt stmt;
            stmt.index_name = std::move(index);
            stmt.table = std::move(table);
            stmt.if_exists = if_exists;
            return stmt;
        }
        throw std::runtime_error("[Parser] Unsupported DROP type: " + kw);
    }

    /**
     * @brief 解析投影列列表
     * @return 投影列列表
     */
    std::vector<SelectStmt::SelectItem> Parser::parse_projections() {
        std::vector<SelectStmt::SelectItem> cols; // 投影列列表
        cols.push_back(parse_select_item());      // 解析第一个投影项
        while (match_text(",")) {
            cols.push_back(parse_select_item()); // 解析后续投影项
        }
        return cols; // 返回投影列列表
    }

    /**
     * @brief 解析单个投影项
     * @return 解析后的投影项
     */
    SelectStmt::SelectItem Parser::parse_select_item() {
        if (match_text("*")) { // 解析通配符*
            SelectStmt::SelectItem item;
            item.value = ColumnRef{ "", "*" };
            return item;
        }
        // 解析表.通配符，如table.*
        if (pos_ + 2 < tokens_.size() && peek_is_identifier_text(tokens_[pos_].text) && tokens_[pos_ + 1].text == "." &&
            tokens_[pos_ + 2].text == "*") {
            std::string tbl = tokens_[pos_].text;
            pos_ += 3; // 跳过表名、点和通配符
            SelectStmt::SelectItem item;
            item.value = ColumnRef{ std::move(tbl), "*" };
            return item;
        }
        SelectStmt::SelectItem item; // 普通投影项
        if (peek_is_identifier()) {
            auto ident = peek_upper(); // 查看标识符
            // 解析聚合函数
            if (ident == "COUNT" || ident == "SUM" || ident == "AVG" || ident == "MIN" || ident == "MAX") {
                item.value = parse_aggregate(); // 解析聚合函数
            } else {
                item.value = parse_expression(); // 解析表达式
            }
        } else {
            item.value = parse_expression(); // 解析表达式
        }

        if (match_keyword("AS")) {
            item.alias = consume_identifier(); // 解析AS别名
        } else if (peek_is_identifier() && !is_projection_reserved(peek_upper())) {
            item.alias = consume_identifier(); // 解析无AS的别名
        }
        return item; // 返回投影项
    }

    /**
     * @brief 解析ORDER BY子句
     * @return ORDER BY项列表
     */
    std::vector<SelectStmt::OrderByItem> Parser::parse_order_by() {
        std::vector<SelectStmt::OrderByItem> items; // ORDER BY项列表

        // 解析排序键的lambda函数
        auto parse_key = [&]() -> std::variant<Expression, AggregateExpr> {
            if (peek_is_identifier()) {
                auto ident = peek_upper(); // 查看标识符
                // 解析聚合函数
                if (ident == "COUNT" || ident == "SUM" || ident == "AVG" || ident == "MIN" || ident == "MAX") {
                    return parse_aggregate(); // 解析聚合函数
                }
            }
            return parse_expression(); // 解析表达式
        };

        auto key = parse_key(); // 解析第一个排序键
        bool asc = true;        // 默认升序
        if (match_keyword("ASC"))
            asc = true; // 升序
        else if (match_keyword("DESC"))
            asc = false;                                                 // 降序
        items.push_back(SelectStmt::OrderByItem{ std::move(key), asc }); // 添加到ORDER BY列表
        while (match_text(",")) {
            auto k = parse_key(); // 解析后续排序键
            bool a = true;        // 默认升序
            if (match_keyword("ASC"))
                a = true; // 升序
            else if (match_keyword("DESC"))
                a = false;                                               // 降序
            items.push_back(SelectStmt::OrderByItem{ std::move(k), a }); // 添加到ORDER BY列表
        }
        return items; // 返回ORDER BY项列表
    }

    /**
     * @brief 解析GROUP BY子句
     * @return GROUP BY列引用列表
     */
    std::vector<ColumnRef> Parser::parse_group_by() {
        std::vector<ColumnRef> cols;        // GROUP BY列列表
        cols.push_back(parse_column_ref()); // 解析第一个列引用
        while (match_text(",")) {
            cols.push_back(parse_column_ref()); // 解析后续列引用
        }
        return cols; // 返回GROUP BY列列表
    }

    /**
     * @brief 解析布尔表达式入口，从OR优先级开始
     * @return 解析后的布尔表达式
     */
    BoolExpr Parser::parse_bool_expr() {
        return parse_or();
    }

    /**
     * @brief 解析OR优先级的布尔表达式
     * @return 解析后的布尔表达式
     */
    BoolExpr Parser::parse_or() {
        auto left = parse_and();      // 解析AND优先级的左操作数
        while (match_keyword("OR")) { // 循环处理OR操作符
            auto node = BoolExpr{};
            node.kind = BoolExpr::Kind::Or;                          // 设置为OR操作
            node.left = std::make_unique<BoolExpr>(std::move(left)); // 左操作数
            node.right = std::make_unique<BoolExpr>(parse_and());    // 右操作数
            left = std::move(node);                                  // 更新左操作数为新节点
        }
        return left; // 返回布尔表达式
    }

    /**
     * @brief 解析AND优先级的布尔表达式
     * @return 解析后的布尔表达式
     */
    BoolExpr Parser::parse_and() {
        auto left = parse_not();       // 解析NOT优先级的左操作数
        while (match_keyword("AND")) { // 循环处理AND操作符
            auto node = BoolExpr{};
            node.kind = BoolExpr::Kind::And;                         // 设置为AND操作
            node.left = std::make_unique<BoolExpr>(std::move(left)); // 左操作数
            node.right = std::make_unique<BoolExpr>(parse_not());    // 右操作数
            left = std::move(node);                                  // 更新左操作数为新节点
        }
        return left; // 返回布尔表达式
    }

    /**
     * @brief 解析NOT优先级的布尔表达式
     * @return 解析后的布尔表达式
     */
    BoolExpr Parser::parse_not() {
        if (match_keyword("NOT")) { // 如果有NOT操作符
            auto node = BoolExpr{};
            node.kind = BoolExpr::Kind::Not;                     // 设置为NOT操作
            node.left = std::make_unique<BoolExpr>(parse_not()); // 操作数
            return node;                                         // 返回NOT表达式
        }
        return parse_bool_atom(); // 解析布尔原子
    }

    /**
     * @brief 解析布尔原子表达式（括号或比较表达式）
     * @return 解析后的布尔表达式
     */
    BoolExpr Parser::parse_bool_atom() {
        // EXISTS (SELECT ...)：非相关子查询，执行前由处理器代换为恒真/恒假；NOT EXISTS 由外层 Not 节点组合。
        if (match_keyword("EXISTS")) {
            expect_text("(");
            Statement sub = parse_select();
            expect_text(")");
            InExpr in_expr;
            in_expr.expr = Literal::from_int(1); // 占位，不参与求值
            in_expr.exists_only = true;
            in_expr.subquery = std::make_shared<SelectStmt>(std::move(std::get<SelectStmt>(sub)));
            return BoolExpr::make_in(std::move(in_expr));
        }
        if (match_text("(")) {             // 解析括号内的表达式
            auto expr = parse_bool_expr(); // 递归解析布尔表达式
            expect_text(")");              // 期望右括号
            return expr;                   // 返回括号内的表达式
        }
        return parse_comparison(); // 解析比较表达式
    }

    /**
     * @brief 解析比较表达式
     * @return 解析后的比较表达式
     *
     * 支持的语法：
     * - 标准比较: expr = expr, expr < expr, etc.
     * - IS NULL / IS NOT NULL: expr IS [NOT] NULL
     * - IN: expr [NOT] IN (value, ...)
     * - BETWEEN: expr [NOT] BETWEEN low AND high
     * - LIKE: expr [NOT] LIKE pattern
     */
    BoolExpr Parser::parse_comparison() {
        auto lhs = parse_expression(); // 解析左操作数

        // 检查 IS NULL / IS NOT NULL
        if (match_keyword("IS")) {
            bool negated = match_keyword("NOT");
            expect_keyword("NULL");

            BoolExpr node;
            node.kind = BoolExpr::Kind::Comparison;
            node.cmp = Comparison{
                std::move(lhs), negated ? CompareOp::IsNotNull : CompareOp::IsNull,
                Literal::null() // 占位符
            };
            return node;
        }

        // 检查 NOT IN / NOT BETWEEN / NOT LIKE
        bool negated = match_keyword("NOT");

        // 检查 IN (value, ...) / IN (SELECT ...)
        if (match_keyword("IN")) {
            expect_text("(");
            InExpr in_expr;
            in_expr.expr = std::move(lhs);
            in_expr.negated = negated;
            if (peek_upper() == "SELECT") {
                // 非相关子查询：执行前由查询处理器求值并代换为字面量 values。
                Statement sub = parse_select();
                in_expr.subquery = std::make_shared<SelectStmt>(std::move(std::get<SelectStmt>(sub)));
            } else {
                std::vector<Expression> values;
                values.push_back(parse_expression());
                while (match_text(",")) {
                    values.push_back(parse_expression());
                }
                in_expr.values = std::move(values);
            }
            expect_text(")");
            return BoolExpr::make_in(std::move(in_expr));
        }

        // 检查 BETWEEN low AND high
        if (match_keyword("BETWEEN")) {
            auto low = parse_expression();
            expect_keyword("AND");
            auto high = parse_expression();

            BetweenExpr between_expr;
            between_expr.expr = std::move(lhs);
            between_expr.low = std::move(low);
            between_expr.high = std::move(high);
            between_expr.negated = negated;
            return BoolExpr::make_between(std::move(between_expr));
        }

        // 检查 LIKE pattern
        if (match_keyword("LIKE")) {
            auto pattern = parse_expression();

            BoolExpr node;
            node.kind = BoolExpr::Kind::Comparison;
            node.cmp = Comparison{ std::move(lhs), negated ? CompareOp::NotLike : CompareOp::Like, std::move(pattern) };
            return node;
        }

        // 如果有孤立的 NOT 没有后续关键字，回退处理
        if (negated) {
            throw std::runtime_error("[Parser] NOT must be followed by IN, BETWEEN, or LIKE");
        }

        auto op_text = consume_operator(); // 解析比较操作符
        auto rhs = parse_expression();     // 解析右操作数

        CompareOp op; // 比较操作符
        if (op_text == "=")
            op = CompareOp::Eq; // 等于
        else if (op_text == "!=" || op_text == "<>")
            op = CompareOp::Ne; // 不等于
        else if (op_text == "<")
            op = CompareOp::Lt; // 小于
        else if (op_text == "<=")
            op = CompareOp::Le; // 小于等于
        else if (op_text == ">")
            op = CompareOp::Gt; // 大于
        else if (op_text == ">=")
            op = CompareOp::Ge; // 大于等于
        else
            throw std::runtime_error("[Parser] Unknown operator: " + op_text);

        BoolExpr node;                                               // 构造布尔表达式节点
        node.kind = BoolExpr::Kind::Comparison;                      // 设置为比较操作
        node.cmp = Comparison{ std::move(lhs), op, std::move(rhs) }; // 设置比较条件
        return node;                                                 // 返回比较表达式
    }

    /**
     * @brief 解析表达式入口，从字符串连接优先级开始
     * @return 解析后的表达式
     *
     * 运算符优先级（从低到高）：
     * 1. || 字符串连接
     * 2. +, - 加减法
     * 3. *, /, % 乘除取模
     * 4. () 括号、函数调用、列引用、字面量
     */
    Expression Parser::parse_expression() {
        auto lhs = parse_additive();     // 解析加法优先级的左操作数
        while (match_text("||")) {       // 解析字符串连接
            auto rhs = parse_additive(); // 解析右操作数
            auto node = std::make_shared<BinaryExpr>();
            node->op = BinaryExpr::Op::Concat; // 设置为字符串连接操作
            node->lhs = std::move(lhs);        // 左操作数
            node->rhs = std::move(rhs);        // 右操作数
            lhs = node;                        // 更新左操作数为新节点
        }
        return lhs;
    }

    /**
     * @brief 解析加法优先级的表达式（+和-）
     * @return 解析后的表达式
     */
    Expression Parser::parse_additive() {
        auto lhs = parse_term(); // 解析乘法优先级的左操作数
        while (true) {
            if (match_text("+")) {       // 解析加法
                auto rhs = parse_term(); // 解析右操作数
                auto node = std::make_shared<BinaryExpr>();
                node->op = BinaryExpr::Op::Add; // 设置为加法操作
                node->lhs = std::move(lhs);     // 左操作数
                node->rhs = std::move(rhs);     // 右操作数
                lhs = node;                     // 更新左操作数为新节点
            } else if (match_text("-")) {       // 解析减法
                auto rhs = parse_term();        // 解析右操作数
                auto node = std::make_shared<BinaryExpr>();
                node->op = BinaryExpr::Op::Sub; // 设置为减法操作
                node->lhs = std::move(lhs);     // 左操作数
                node->rhs = std::move(rhs);     // 右操作数
                lhs = node;                     // 更新左操作数为新节点
            } else {
                break; // 没有更多加法/减法操作符
            }
        }
        return lhs; // 返回表达式
    }

    /**
     * @brief 解析乘法优先级的表达式（*、/、%）
     * @return 解析后的表达式
     */
    Expression Parser::parse_term() {
        auto lhs = parse_factor(); // 解析因子优先级的左操作数
        while (true) {
            if (match_text("*")) {         // 解析乘法
                auto rhs = parse_factor(); // 解析右操作数
                auto node = std::make_shared<BinaryExpr>();
                node->op = BinaryExpr::Op::Mul; // 设置为乘法操作
                node->lhs = std::move(lhs);     // 左操作数
                node->rhs = std::move(rhs);     // 右操作数
                lhs = node;                     // 更新左操作数为新节点
            } else if (match_text("/")) {       // 解析除法
                auto rhs = parse_factor();      // 解析右操作数
                auto node = std::make_shared<BinaryExpr>();
                node->op = BinaryExpr::Op::Div; // 设置为除法操作
                node->lhs = std::move(lhs);     // 左操作数
                node->rhs = std::move(rhs);     // 右操作数
                lhs = node;                     // 更新左操作数为新节点
            } else if (match_text("%")) {       // 解析取模
                auto rhs = parse_factor();      // 解析右操作数
                auto node = std::make_shared<BinaryExpr>();
                node->op = BinaryExpr::Op::Mod; // 设置为取模操作
                node->lhs = std::move(lhs);     // 左操作数
                node->rhs = std::move(rhs);     // 右操作数
                lhs = node;                     // 更新左操作数为新节点
            } else {
                break; // 没有更多乘法/除法/取模操作符
            }
        }
        return lhs; // 返回表达式
    }

    /**
     * @brief 解析因子表达式（括号、函数、列引用或字面量）
     * @return 解析后的表达式
     */
    Expression Parser::parse_factor() {
        if (match_text("(")) {               // 解析括号内的表达式
            auto inner = parse_expression(); // 递归解析表达式
            expect_text(")");                // 期望右括号
            return inner;                    // 返回括号内的表达式
        }
        if (peek_is_identifier()) {
            auto ident = peek_upper(); // 查看标识符
            // NULL / TRUE / FALSE 字面量（SQL 保留字，不作列名）
            if (ident == "NULL") {
                consume();
                return Literal::null();
            }
            if (ident == "TRUE") {
                consume();
                return Literal::from_int(1);
            }
            if (ident == "FALSE") {
                consume();
                return Literal::from_int(0);
            }
            // 解析聚合函数
            if (ident == "COUNT" || ident == "SUM" || ident == "AVG" || ident == "MIN" || ident == "MAX") {
                return parse_aggregate(); // 解析聚合函数
            }
            // 解析标量函数
            if (ident == "COALESCE" || ident == "NULLIF" || ident == "UPPER" || ident == "LOWER" || ident == "LENGTH" ||
                ident == "TRIM" || ident == "SUBSTR" || ident == "ABS") {
                return parse_scalar_function(); // 解析标量函数
            }
            return parse_column_ref(); // 解析列引用
        }
        return Literal{ parse_literal() }; // 解析字面量
    }

    /**
     * @brief 解析标量函数表达式
     * @return 解析后的标量函数表达式
     *
     * 支持的函数：
     * - COALESCE(expr, expr, ...): 返回第一个非NULL值
     * - NULLIF(expr1, expr2): 如果expr1=expr2返回NULL，否则返回expr1
     * - UPPER(str): 转大写
     * - LOWER(str): 转小写
     * - LENGTH(str): 字符串长度
     * - TRIM(str): 去除首尾空白
     * - SUBSTR(str, start, len): 子字符串
     * - ABS(num): 绝对值
     */
    Expression Parser::parse_scalar_function() {
        std::string func_name = to_upper(consume_identifier()); // 解析函数名
        expect_text("(");                                       // 期望左括号

        // 确定函数类型
        ScalarFunc func;
        if (func_name == "COALESCE")
            func = ScalarFunc::Coalesce;
        else if (func_name == "NULLIF")
            func = ScalarFunc::Nullif;
        else if (func_name == "UPPER")
            func = ScalarFunc::Upper;
        else if (func_name == "LOWER")
            func = ScalarFunc::Lower;
        else if (func_name == "LENGTH")
            func = ScalarFunc::Length;
        else if (func_name == "TRIM")
            func = ScalarFunc::Trim;
        else if (func_name == "SUBSTR")
            func = ScalarFunc::Substr;
        else if (func_name == "ABS")
            func = ScalarFunc::Abs;
        else
            throw std::runtime_error("[Parser] Unknown scalar function: " + func_name);

        // 解析参数列表
        std::vector<Expression> args;
        if (!match_text(")")) {                 // 如果有参数
            args.push_back(parse_expression()); // 解析第一个参数
            while (match_text(",")) {
                args.push_back(parse_expression()); // 解析后续参数
            }
            expect_text(")"); // 期望右括号
        }

        // 参数个数校验
        switch (func) {
            case ScalarFunc::Nullif:
                if (args.size() != 2)
                    throw std::runtime_error("[Parser] NULLIF requires exactly 2 arguments");
                break;
            case ScalarFunc::Upper:
            case ScalarFunc::Lower:
            case ScalarFunc::Length:
            case ScalarFunc::Trim:
            case ScalarFunc::Abs:
                if (args.size() != 1)
                    throw std::runtime_error("[Parser] " + func_name + " requires exactly 1 argument");
                break;
            case ScalarFunc::Substr:
                if (args.size() < 2 || args.size() > 3)
                    throw std::runtime_error("[Parser] SUBSTR requires 2 or 3 arguments");
                break;
            case ScalarFunc::Coalesce:
                if (args.empty())
                    throw std::runtime_error("[Parser] COALESCE requires at least 1 argument");
                break;
        }

        // 构造函数表达式
        auto func_expr = std::make_shared<FunctionExpr>();
        func_expr->func = func;
        func_expr->args = std::move(args);
        return func_expr;
    }

    /**
     * @brief 解析字面量表达式
     * @return 解析后的字面量表达式
     */
    Expression Parser::parse_literal_expression() {
        return Literal{ parse_literal() };
    }

    /**
     * @brief 解析更新赋值
     * @return 解析后的更新赋值
     */
    UpdateAssign Parser::parse_assignment() {
        std::string col = consume_identifier();                 // 解析列名
        expect_text("=");                                       // 期望等号
        auto expr = parse_expression();                         // 解析赋值表达式
        return UpdateAssign{ std::move(col), std::move(expr) }; // 返回更新赋值
    }

    /**
     * @brief 解析聚合函数
     * @return 解析后的聚合表达式
     */
    AggregateExpr Parser::parse_aggregate() {
        std::string func = to_upper(consume_identifier()); // 解析函数名
        expect_text("(");                                  // 期望左括号
        if (match_text("*")) {                             // 解析COUNT(*)
            expect_text(")");                              // 期望右括号
            AggFunc f = AggFunc::Count;                    // 默认COUNT
            if (func == "SUM")
                f = AggFunc::Sum; // SUM函数
            else if (func == "AVG")
                f = AggFunc::Avg; // AVG函数
            else if (func == "MIN")
                f = AggFunc::Min; // MIN函数
            else if (func == "MAX")
                f = AggFunc::Max;                    // MAX函数
            return AggregateExpr{ f, std::nullopt }; // 返回聚合表达式
        }
        auto col = parse_column_ref(); // 解析列引用，如COUNT(col)
        expect_text(")");              // 期望右括号
        AggFunc f = AggFunc::Count;    // 默认COUNT
        if (func == "SUM")
            f = AggFunc::Sum; // SUM函数
        else if (func == "AVG")
            f = AggFunc::Avg; // AVG函数
        else if (func == "MIN")
            f = AggFunc::Min; // MIN函数
        else if (func == "MAX")
            f = AggFunc::Max;           // MAX函数
        return AggregateExpr{ f, col }; // 返回聚合表达式
    }

    /**
     * @brief 解析列引用
     * @return 解析后的列引用
     */
    ColumnRef Parser::parse_column_ref() {
        std::string ident = consume_identifier();                 // 解析标识符
        if (match_text(".")) {                                    // 如果有表名限定，如table.column
            std::string col = consume_identifier();               // 解析列名
            return ColumnRef{ std::move(ident), std::move(col) }; // 返回表.列引用
        }
        return ColumnRef{ "", std::move(ident) }; // 返回仅列名的引用
    }

    /**
     * @brief 解析字面量（整数或字符串）
     * @return 解析后的字面量值
     */
    Value Parser::parse_literal() {
        const auto& tok = consume(); // 消费当前token
        // NULL / TRUE / FALSE 字面量
        const std::string up = to_upper(tok.text);
        if (up == "NULL") {
            return Value{ NullValue{} };
        }
        if (up == "TRUE") {
            return Value{ static_cast<int64_t>(1) };
        }
        if (up == "FALSE") {
            return Value{ static_cast<int64_t>(0) };
        }
        // 解析整数或浮点字面量
        if (!tok.text.empty() && (std::isdigit(static_cast<unsigned char>(tok.text[0])) ||
                                  (tok.text[0] == '-' && tok.text.size() > 1))) {
            if (tok.text.find('.') != std::string::npos)
                return Value{ std::stod(tok.text) };
            return Value{ static_cast<int64_t>(std::stoll(tok.text)) }; // 返回整数值
        }
        // 解析字符串字面量（单引号包围）
        if (tok.text.size() >= 2 && tok.text.front() == '\'' && tok.text.back() == '\'') {
            return Value{ tok.text.substr(1, tok.text.size() - 2) }; // 返回字符串值（去除单引号）
        }
        throw std::runtime_error("[Parser] Invalid literal: " + tok.text);
    }

    /**
     * @brief 解析整数字面量
     * @return 解析后的整数值
     */
    int64_t Parser::parse_int_literal() {
        const auto& tok = consume(); // 消费当前token
        // 检查是否为整数
        if (!tok.text.empty() && std::isdigit(static_cast<unsigned char>(tok.text[0]))) {
            return static_cast<int64_t>(std::stoll(tok.text)); // 返回整数值
        }
        throw std::runtime_error("[Parser] Expected integer literal, got " + tok.text);
    }

    /**
     * @brief 将SQL字符串分词
     * @param sql 要分词的SQL字符串
     * @return 分词后的token列表
     */
    std::vector<Parser::Token> Parser::tokenize(const std::string& sql) {
        std::vector<Token> out; // token输出列表
        std::string current;    // 当前token文本
        // 刷新当前token到输出列表
        auto flush = [&]() {
            if (!current.empty()) {
                out.push_back({ current }); // 添加到输出列表
                current.clear();            // 清空当前token
            }
        };
        // 添加单个字符到输出列表
        auto push_one = [&](char ch) { out.push_back({ std::string(1, ch) }); };

        for (size_t i = 0; i < sql.size(); ++i) { // 遍历SQL字符串
            char c = sql[i];
            if (std::isspace(static_cast<unsigned char>(c))) { // 跳过空白字符
                flush();                                       // 刷新当前token
                continue;
            }
            if (c == '\'') { // 解析字符串字面量
                flush();     // 刷新当前token
                std::string literal;
                literal.push_back(c); // 添加起始单引号
                ++i;
                while (i < sql.size()) {
                    if (sql[i] == '\'') {
                        // SQL standard: two consecutive single quotes → one literal quote.
                        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                            literal.push_back('\'');
                            i += 2;
                            continue;
                        }
                        break; // closing quote
                    }
                    literal.push_back(sql[i]);
                    ++i;
                }
                if (i >= sql.size())
                    throw std::runtime_error("[Parser] Unterminated string literal");
                literal.push_back('\'');    // 添加结束单引号
                out.push_back({ literal }); // 添加到输出列表
                continue;
            }

            // 解析双字符比较操作符
            if ((c == '!' || c == '<' || c == '>') && i + 1 < sql.size()) {
                char n = sql[i + 1];
                if (n == '=') { // 解析!=, <=, >=
                    flush();
                    out.push_back({ std::string{ c, n } }); // 添加双字符操作符
                    ++i;                                    // 跳过下一个字符
                    continue;
                }
                if (c == '<' && n == '>') { // 解析<>
                    flush();
                    out.push_back({ "<>" }); // 添加<>操作符
                    ++i;                     // 跳过下一个字符
                    continue;
                }
            }

            // 解析单字符token（但小数点后跟数字时属于浮点字面量）
            if (c == '.' && !current.empty() && std::isdigit(static_cast<unsigned char>(current.back())) &&
                i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
                current.push_back(c); // 小数点并入数字 token
                continue;
            }
            if (c == '(' || c == ')' || c == ',' || c == ';' || c == '.' || c == '*' || c == '+' || c == '-' ||
                c == '/' || c == '=' || c == '?' || c == '%') {
                flush();     // 刷新当前token
                push_one(c); // 添加单字符token
                continue;
            }

            current.push_back(c); // 添加到当前token
        }
        flush();    // 刷新最后一个token
        return out; // 返回token列表
    }

    /**
     * @brief 消费当前token并返回
     * @return 当前token的引用
     */
    const Parser::Token& Parser::consume() {
        if (pos_ >= tokens_.size())
            throw std::runtime_error("[Parser] Unexpected end of input");
        return tokens_[pos_++]; // 返回当前token并移动到下一个
    }

    /**
     * @brief 消费标识符token
     * @return 标识符文本
     */
    std::string Parser::consume_identifier() {
        const auto& t = consume();              // 消费当前token
        if (!peek_is_identifier_text(t.text)) { // 检查是否为标识符
            throw std::runtime_error("[Parser] Expected identifier, got " + t.text);
        }
        return t.text; // 返回标识符文本
    }

    /**
     * @brief 消费比较操作符token
     * @return 操作符文本
     */
    std::string Parser::consume_operator() {
        const auto& t = consume(); // 消费当前token
        // 检查是否为有效的比较操作符
        if (!(t.text == "=" || t.text == "!=" || t.text == "<>" || t.text == "<" || t.text == "<=" || t.text == ">" ||
              t.text == ">=")) {
            throw std::runtime_error("[Parser] Expected comparison operator, got: " + t.text);
        }
        return t.text; // 返回操作符文本
    }

    /**
     * @brief 匹配指定文本的token
     * @param text 要匹配的文本
     * @return 如果匹配成功返回true，否则返回false
     */
    bool Parser::match_text(std::string_view text) {
        if (pos_ < tokens_.size() && tokens_[pos_].text == text) {
            ++pos_; // 匹配成功，移动到下一个token
            return true;
        }
        return false; // 匹配失败
    }

    /**
     * @brief 匹配指定关键字
     * @param kw 要匹配的关键字
     * @return 如果匹配成功返回true，否则返回false
     */
    bool Parser::match_keyword(std::string_view kw) {
        if (pos_ < tokens_.size() && to_upper(tokens_[pos_].text) == kw) {
            ++pos_; // 匹配成功，移动到下一个token
            return true;
        }
        return false; // 匹配失败
    }

    /**
     * @brief 期望指定关键字，不匹配则抛出异常
     * @param kw 期望的关键字
     */
    void Parser::expect_keyword(std::string_view kw) {
        if (!match_keyword(kw))
            throw std::runtime_error("[Parser] Expected keyword: " + std::string(kw));
    }

    /**
     * @brief 期望指定文本，不匹配则抛出异常
     * @param text 期望的文本
     */
    void Parser::expect_text(std::string_view text) {
        if (!match_text(text))
            throw std::runtime_error("[Parser] Expected token " + std::string(text));
    }

    /**
     * @brief 检查当前token是否为标识符
     * @return 如果是标识符返回true，否则返回false
     */
    bool Parser::peek_is_identifier() const {
        return pos_ < tokens_.size() && peek_is_identifier_text(tokens_[pos_].text); // 检查当前token是否为标识符
    }

    /**
     * @brief 检查文本是否为标识符
     * @param text 要检查的文本
     * @return 如果是标识符返回true，否则返回false
     */
    bool Parser::peek_is_identifier_text(const std::string& text) {
        return !text.empty() &&
               (std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'); // 标识符以字母或下划线开头
    }

    /**
     * @brief 查看当前token的大写形式
     * @return 当前token的大写文本
     */
    std::string Parser::peek_upper() const {
        if (pos_ >= tokens_.size())
            return "";                       // 没有更多token
        return to_upper(tokens_[pos_].text); // 返回当前token的大写形式
    }

    /**
     * @brief 解析BEGIN语句
     * @return 解析后的BeginStmt对象
     *
     * 语法: BEGIN [TRANSACTION]
     */
    BeginStmt Parser::parse_begin() {
        consume(); // 消费 BEGIN
        // 可选: TRANSACTION 关键字
        if (peek_upper() == "TRANSACTION") {
            consume();
        }
        return BeginStmt{};
    }

    /**
     * @brief 解析COMMIT语句
     * @return 解析后的CommitStmt对象
     *
     * 语法: COMMIT [TRANSACTION]
     */
    CommitStmt Parser::parse_commit() {
        consume(); // 消费 COMMIT
        // 可选: TRANSACTION 关键字
        if (peek_upper() == "TRANSACTION") {
            consume();
        }
        return CommitStmt{};
    }

    /**
     * @brief 解析ROLLBACK语句
     * @return 解析后的RollbackStmt对象
     *
     * 语法: ROLLBACK [TRANSACTION]
     */
    RollbackStmt Parser::parse_rollback() {
        consume(); // 消费 ROLLBACK
        // ROLLBACK TO [SAVEPOINT] name：回滚到保存点，事务继续。
        if (match_keyword("TO")) {
            match_keyword("SAVEPOINT"); // 可选
            RollbackStmt stmt;
            stmt.savepoint = consume_identifier();
            return stmt;
        }
        // 可选: TRANSACTION 关键字
        if (peek_upper() == "TRANSACTION") {
            consume();
        }
        return RollbackStmt{};
    }

    CheckpointStmt Parser::parse_checkpoint() {
        consume(); // consume CHECKPOINT
        return CheckpointStmt{};
    }

    ShowStatusStmt Parser::parse_show() {
        consume(); // consume SHOW
        auto kw = peek_upper();
        if (kw != "STATUS")
            throw std::runtime_error("[Parser] SHOW expects STATUS");
        consume(); // consume STATUS
        return ShowStatusStmt{};
    }

    PrepareStmt Parser::parse_prepare() {
        consume(); // consume PREPARE
        std::string name = consume_identifier();
        // Expect FROM keyword (optional, for MySQL compatibility)
        if (peek_upper() == "FROM")
            consume();
        // Get the SQL text: expect a string literal
        const auto& tok = consume();
        if (tok.text.size() < 2 || tok.text.front() != '\'' || tok.text.back() != '\'')
            throw std::runtime_error("[Parser] PREPARE requires a SQL string literal");
        std::string sql = tok.text.substr(1, tok.text.size() - 2);
        return PrepareStmt{ std::move(name), std::move(sql) };
    }

    ExecuteStmt Parser::parse_execute() {
        consume(); // consume EXECUTE
        std::string name = consume_identifier();
        std::vector<Value> params;
        if (match_text("(")) {
            params.push_back(parse_literal());
            while (match_text(","))
                params.push_back(parse_literal());
            expect_text(")");
        }
        return ExecuteStmt{ std::move(name), std::move(params) };
    }

    DeallocateStmt Parser::parse_deallocate() {
        consume(); // consume DEALLOCATE
        auto kw = peek_upper();
        if (kw != "PREPARE" && kw != "ALL")
            throw std::runtime_error("[Parser] DEALLOCATE expects PREPARE or ALL");
        consume(); // consume PREPARE / ALL
        std::string name;
        if (kw == "PREPARE") {
            name = consume_identifier();
        }
        // kw == "ALL": name stays empty → deallocate all
        return DeallocateStmt{ std::move(name) };
    }

    AuthStmt Parser::parse_auth() {
        consume(); // consume AUTH
        std::string username = consume_identifier();
        // Expect password as string literal
        const auto& tok = consume();
        if (tok.text.size() < 2 || tok.text.front() != '\'' || tok.text.back() != '\'')
            throw std::runtime_error("[Parser] AUTH requires a password string literal");
        std::string password = tok.text.substr(1, tok.text.size() - 2);
        return AuthStmt{ std::move(username), std::move(password) };
    }

    CreateUserStmt Parser::parse_create_user() {
        consume(); // consume USER
        std::string username = consume_identifier();
        // Expect password as string literal
        const auto& tok = consume();
        if (tok.text.size() < 2 || tok.text.front() != '\'' || tok.text.back() != '\'')
            throw std::runtime_error("[Parser] CREATE USER requires a password string literal");
        std::string password = tok.text.substr(1, tok.text.size() - 2);
        return CreateUserStmt{ std::move(username), std::move(password) };
    }

    /**
     * @brief 解析 SET TRANSACTION ISOLATION LEVEL 语句
     *
     * 语法:
     *   SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED
     *   SET TRANSACTION ISOLATION LEVEL READ COMMITTED
     *   SET TRANSACTION ISOLATION LEVEL REPEATABLE READ
     *   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
     *
     * 当前实现仅记录到 Session 中，可见性差异在 MVCC 阶段补全。
     */
    SetTransactionStmt Parser::parse_set() {
        consume(); // SET
        if (peek_upper() != "TRANSACTION") {
            throw std::runtime_error("[Parser] Only SET TRANSACTION ... is supported");
        }
        consume(); // TRANSACTION
        if (peek_upper() != "ISOLATION") {
            throw std::runtime_error("[Parser] Expected ISOLATION after SET TRANSACTION");
        }
        consume(); // ISOLATION
        if (peek_upper() != "LEVEL") {
            throw std::runtime_error("[Parser] Expected LEVEL after ISOLATION");
        }
        consume(); // LEVEL

        auto first = peek_upper();
        consume();
        SetTransactionStmt out;
        if (first == "READ") {
            auto second = peek_upper();
            consume();
            if (second == "UNCOMMITTED")
                out.isolation_level = 0;
            else if (second == "COMMITTED")
                out.isolation_level = 1;
            else
                throw std::runtime_error("[Parser] Unknown isolation: READ " + second);
        } else if (first == "REPEATABLE") {
            auto second = peek_upper();
            consume();
            if (second != "READ")
                throw std::runtime_error("[Parser] Expected REPEATABLE READ");
            out.isolation_level = 2;
        } else if (first == "SERIALIZABLE") {
            out.isolation_level = 3;
        } else {
            throw std::runtime_error("[Parser] Unknown isolation level: " + first);
        }
        return out;
    }

} // namespace corodb
