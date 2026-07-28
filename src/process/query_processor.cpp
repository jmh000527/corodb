// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file query_processor.cpp
// @brief 顶层 SQL 流水线（PG 风格 exec_simple_query）实现。

#include "corodb/process/query_processor.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>
#include <utility>

#include "corodb/common/config.h"
#include "corodb/db/database.h"
#include "corodb/executor/executor.h"
#include "corodb/plan/logical_plan.h"
#include "corodb/process/explain_printer.h"

namespace corodb {

    namespace {

        // 从语句中提取涉及的表名列表
        std::vector<std::string> extract_table_names(const Statement& stmt) {
            std::vector<std::string> tables;
            if (std::holds_alternative<SelectStmt>(stmt)) {
                const auto& s = std::get<SelectStmt>(stmt);
                tables.push_back(s.from_table);
                for (const auto& join: s.joins) {
                    tables.push_back(join.table);
                }
            } else if (std::holds_alternative<InsertStmt>(stmt)) {
                tables.push_back(std::get<InsertStmt>(stmt).table);
            } else if (std::holds_alternative<UpdateStmt>(stmt)) {
                tables.push_back(std::get<UpdateStmt>(stmt).table);
            } else if (std::holds_alternative<DeleteStmt>(stmt)) {
                tables.push_back(std::get<DeleteStmt>(stmt).table);
            } else if (std::holds_alternative<CreateStmt>(stmt)) {
                tables.push_back(std::get<CreateStmt>(stmt).table);
            } else if (std::holds_alternative<CreateIndexStmt>(stmt)) {
                tables.push_back(std::get<CreateIndexStmt>(stmt).table);
            }
            return tables;
        }

        // 判断语句是否为 DDL（CREATE TABLE / CREATE INDEX）
        bool is_ddl(const Statement& stmt) {
            return std::holds_alternative<CreateStmt>(stmt) || std::holds_alternative<CreateIndexStmt>(stmt);
        }

        // ---- 相关子查询支持：作用域/代换/检测 ----

        // 子查询自身 FROM 集（表名与别名）。
        std::unordered_set<std::string> subquery_local_names(const SelectStmt& s) {
            std::unordered_set<std::string> names;
            names.insert(s.from_table);
            if (s.from_alias.has_value())
                names.insert(*s.from_alias);
            for (const auto& j: s.joins) {
                names.insert(j.table);
                if (j.alias.has_value())
                    names.insert(*j.alias);
            }
            return names;
        }

        // 外层行中查找 (表/别名, 列) 绑定的值。
        std::optional<Value> outer_lookup(const Record& outer, const std::string& table, const std::string& col) {
            for (std::size_t i = 0; i < outer.bindings.size(); ++i) {
                if (outer.bindings[i].table == table && outer.bindings[i].column == col)
                    return outer.values[i];
            }
            return std::nullopt;
        }

        void substitute_outer_refs_bool(BoolExpr& b, const std::unordered_set<std::string>& local,
                                        const Record& outer);

        // 表达式代换：表限定不属于 local 的列引用 → 外层行字面量；共享节点先克隆再改。
        void substitute_outer_refs_expr(Expression& e, const std::unordered_set<std::string>& local,
                                        const Record& outer) {
            if (auto* ref = std::get_if<ColumnRef>(&e)) {
                if (!ref->table.empty() && local.count(ref->table) == 0) {
                    if (auto v = outer_lookup(outer, ref->table, ref->name))
                        e = Literal{ *v };
                    // 找不到则保留，规划期报未知列（错误信息清晰）。
                }
                return;
            }
            if (auto* bin = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
                if (!*bin)
                    return;
                auto clone = std::make_shared<BinaryExpr>(**bin);
                substitute_outer_refs_expr(clone->lhs, local, outer);
                substitute_outer_refs_expr(clone->rhs, local, outer);
                e = std::move(clone);
                return;
            }
            if (auto* fn = std::get_if<std::shared_ptr<FunctionExpr>>(&e)) {
                if (!*fn)
                    return;
                auto clone = std::make_shared<FunctionExpr>(**fn);
                for (auto& a: clone->args)
                    substitute_outer_refs_expr(a, local, outer);
                e = std::move(clone);
                return;
            }
        }

        // 布尔树代换；嵌套子查询克隆后在“local ∪ 其自身 FROM 集”作用域下继续代换最外层引用。
        void substitute_outer_refs_bool(BoolExpr& b, const std::unordered_set<std::string>& local,
                                        const Record& outer) {
            if (b.left)
                substitute_outer_refs_bool(*b.left, local, outer);
            if (b.right)
                substitute_outer_refs_bool(*b.right, local, outer);
            if (b.cmp.has_value()) {
                substitute_outer_refs_expr(b.cmp->lhs, local, outer);
                substitute_outer_refs_expr(b.cmp->rhs, local, outer);
            }
            if (b.in_expr.has_value()) {
                substitute_outer_refs_expr(b.in_expr->expr, local, outer);
                for (auto& v: b.in_expr->values)
                    substitute_outer_refs_expr(v, local, outer);
                if (b.in_expr->subquery) {
                    auto clone = std::make_shared<SelectStmt>(*b.in_expr->subquery);
                    auto inner_local = local;
                    for (const auto& n: subquery_local_names(*clone))
                        inner_local.insert(n);
                    if (clone->where.has_value())
                        substitute_outer_refs_bool(*clone->where, inner_local, outer);
                    b.in_expr->subquery = std::move(clone);
                }
            }
            if (b.between_expr.has_value()) {
                substitute_outer_refs_expr(b.between_expr->expr, local, outer);
                substitute_outer_refs_expr(b.between_expr->low, local, outer);
                substitute_outer_refs_expr(b.between_expr->high, local, outer);
            }
        }

        void detect_outer_refs_bool(const BoolExpr& b, const std::unordered_set<std::string>& local, bool& found);

        // 相关性检测：表限定且不属于（自身 ∪ 嵌套自身）FROM 集的列引用。
        void detect_outer_refs_expr(const Expression& e, const std::unordered_set<std::string>& local, bool& found) {
            if (const auto* ref = std::get_if<ColumnRef>(&e)) {
                if (!ref->table.empty() && local.count(ref->table) == 0)
                    found = true;
                return;
            }
            if (const auto* bin = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
                if (*bin) {
                    detect_outer_refs_expr((*bin)->lhs, local, found);
                    detect_outer_refs_expr((*bin)->rhs, local, found);
                }
                return;
            }
            if (const auto* fn = std::get_if<std::shared_ptr<FunctionExpr>>(&e)) {
                if (*fn)
                    for (const auto& a: (*fn)->args)
                        detect_outer_refs_expr(a, local, found);
            }
        }

        void detect_outer_refs_bool(const BoolExpr& b, const std::unordered_set<std::string>& local, bool& found) {
            if (b.left)
                detect_outer_refs_bool(*b.left, local, found);
            if (b.right)
                detect_outer_refs_bool(*b.right, local, found);
            if (b.cmp.has_value()) {
                detect_outer_refs_expr(b.cmp->lhs, local, found);
                detect_outer_refs_expr(b.cmp->rhs, local, found);
            }
            if (b.in_expr.has_value()) {
                detect_outer_refs_expr(b.in_expr->expr, local, found);
                for (const auto& v: b.in_expr->values)
                    detect_outer_refs_expr(v, local, found);
                if (b.in_expr->subquery && b.in_expr->subquery->where.has_value()) {
                    auto inner_local = local;
                    for (const auto& n: subquery_local_names(*b.in_expr->subquery))
                        inner_local.insert(n);
                    detect_outer_refs_bool(*b.in_expr->subquery->where, inner_local, found);
                }
            }
            if (b.between_expr.has_value()) {
                detect_outer_refs_expr(b.between_expr->expr, local, found);
                detect_outer_refs_expr(b.between_expr->low, local, found);
                detect_outer_refs_expr(b.between_expr->high, local, found);
            }
        }

    } // namespace

    /**
     * @brief 相关子查询运行器：按外层行代换外层引用为字面量后递归规划/执行（nested apply）。
     *
     * 逐外层行执行（O(N×M)），正确性优先；引用外层列须带表名/别名限定。
     */
    class QueryProcessor::CorrelatedRunner final : public SubqueryRunner {
    public:
        CorrelatedRunner(QueryProcessor& qp, std::shared_ptr<Session> session)
            : qp_(qp), session_(std::move(session)) {
        }

        std::vector<Value> run_subquery(const SelectStmt& sub, const Record& outer, bool exists_only) override {
            SelectStmt resolved{ sub };
            auto local = subquery_local_names(resolved);
            if (resolved.where.has_value())
                substitute_outer_refs_bool(*resolved.where, local, outer);
            Statement stmt{ std::move(resolved) };
            auto plan = qp_.build_physical_plan(stmt);
            ExecutionContext ctx{ session_, &qp_.row_locks_, &qp_.catalog_, &qp_.storage_, &qp_.txn_manager_ };
            ctx.subquery_runner = this; // 支持嵌套相关子查询
            Executor ex{ ctx };
            std::vector<Value> out;
            auto gen = ex.run(plan.get());
            for (auto&& rec: gen) {
                if (exists_only) {
                    out.push_back(Value{ static_cast<int64_t>(1) });
                    break; // 存在性短路
                }
                if (rec.values.size() != 1)
                    throw std::runtime_error("[Process] IN subquery must return exactly one column");
                out.push_back(rec.values.front());
            }
            return out;
        }

    private:
        QueryProcessor& qp_;
        std::shared_ptr<Session> session_;
    };

    std::generator<Record> QueryProcessor::build_status_rows() {
        std::vector<Binding> bindings;
        bindings.push_back(Binding{ "", "metric", 0 });
        bindings.push_back(Binding{ "", "value", 1 });

        // Use the existing session's txn context. For a default session, txn_id=0.
        auto make_row = [&](std::string metric, std::string value) -> Record {
            Record r;
            r.bindings = bindings;
            r.values.push_back(std::move(metric));
            r.values.push_back(std::move(value));
            return r;
        };

        co_yield make_row("active_transactions", std::to_string(txn_manager_.active_count()));
        co_yield make_row("plan_cache_entries", std::to_string(plan_cache_.size()));
        co_yield make_row("tables", std::to_string(catalog_.size()));
        co_yield make_row("isolation_level", "see session");
        co_yield make_row("storage_engine", "LSM-Tree");
        co_yield make_row("database_version", "0.1.0");
    }

    // ---- PlanCache ----

    std::shared_ptr<PlanNode> PlanCache::lookup(const std::string& key) {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(key);
        if (it == cache_.end())
            return nullptr;
        // 移到 MRU 位置
        auto lit = std::find(lru_.begin(), lru_.end(), key);
        if (lit != lru_.end()) {
            lru_.erase(lit);
            lru_.push_back(key);
        }
        return it->second;
    }

    void PlanCache::insert(const std::string& key, std::shared_ptr<PlanNode> plan) {
        std::lock_guard lock(mutex_);
        // 容量满时淘汰 LRU 条目。
        while (cache_.size() >= max_entries_ && !lru_.empty()) {
            const std::string& lru_key = lru_.front();
            cache_.erase(lru_key);
            lru_.pop_front();
        }
        cache_[key] = std::move(plan);
        lru_.push_back(key);
    }

    void PlanCache::invalidate_all() {
        std::lock_guard lock(mutex_);
        cache_.clear();
        lru_.clear();
    }

    std::size_t PlanCache::size() const {
        std::lock_guard lock(mutex_);
        return cache_.size();
    }

    std::string QueryProcessor::normalize_sql(const std::string& sql) {
        std::string out;
        out.reserve(sql.size());
        bool in_space = false;
        for (char c : sql) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!in_space) {
                    out.push_back(' ');
                    in_space = true;
                }
            } else {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                in_space = false;
            }
        }
        // 去除首尾空格。
        auto start = out.find_first_not_of(' ');
        if (start == std::string::npos)
            return "";
        auto end = out.find_last_not_of(' ');
        return out.substr(start, end - start + 1);
    }

    /**
     * @brief 构造 QueryProcessor，绑定 Catalog、存储引擎、事务管理器及锁组件。
     */
    QueryProcessor::QueryProcessor(Catalog& catalog, StorageEngine& storage, TransactionManager& txn_manager,
                                   LockManager& lock_manager, RowLockManager& row_locks, std::mutex& commit_apply_mutex,
                                   UserManager& user_manager)
        : catalog_(catalog), storage_(storage), txn_manager_(txn_manager), lock_manager_(lock_manager),
          row_locks_(row_locks), txn_ctrl_(txn_manager, catalog, storage, row_locks, commit_apply_mutex),
          utility_(catalog, storage), user_manager_(user_manager) {
        plan_cache_.set_max_entries(Config::instance().plan_cache_entries());
    }

    /**
     * @brief 对语句完整执行逻辑优化与物理规划，返回物理执行计划树。
     * @param stmt 已解析的 AST 语句。
     * @return 物理计划根节点的 unique_ptr。
     */
    std::unique_ptr<PlanNode> QueryProcessor::build_physical_plan(const Statement& stmt) {
        // UNION：各臂独立规划后拼为 UnionPlan（去重语义由执行器实现）。
        if (const auto* sel = std::get_if<SelectStmt>(&stmt); sel && !sel->unions.empty()) {
            std::vector<std::unique_ptr<PlanNode>> arms;
            arms.reserve(sel->unions.size() + 1);
            SelectStmt head{ *sel };
            head.unions.clear();
            const bool all = sel->unions[0].all;
            arms.push_back(build_physical_plan(Statement{ std::move(head) }));
            for (const auto& u: sel->unions) {
                SelectStmt arm_stmt{ *u.select };
                arm_stmt.unions.clear(); // 解析期已放平；防御性清空
                arms.push_back(build_physical_plan(Statement{ std::move(arm_stmt) }));
            }
            return std::make_unique<UnionPlan>(std::move(arms), all);
        }
        opt::LogicalPlanner lplanner{ catalog_ };
        auto lp = lplanner.plan(stmt);
        opt::RuleSet rules = opt::make_default_rules();
        lp = rules.apply(std::move(lp));
        opt::PhysicalPlanner pplanner{ catalog_, &storage_ };
        return pplanner.plan(*lp);
    }

    /**
     * @brief 顶层 SQL 执行入口：解析 → 事务控制分发 → 规划 → DDL/DML/SELECT 执行。
     * @param sql 原始 SQL 字符串。
     * @param session 当前会话（含事务状态与写缓冲）。
     * @return 包含结果行集或状态消息的 ProcessedQuery。
     */
    /**
     * @brief 顶层 SQL 执行入口（PG 风格 exec_simple_query）。
     *
     * 流水线：解析 → 事务控制分发 → 规划 → DDL/DML/SELECT 分派。
     *
     * 与 PostgreSQL 的 exec_simple_query() 对应——从 SQL 字符串到结果行的
     * 完整路径都在此方法内。主要阶段：
     *
     *   1. Parser：SQL 文本 → AST（递归下降解析器）
     *   2. 事务控制：BEGIN/COMMIT/ROLLBACK/SET 在此层处理，不进入执行器
     *   3. LogicalPlanner：AST → LogicalPlan 树（从语义构建算子流水线）
     *   4. RuleSet：定点迭代重写 LogicalPlan（5 条规则，最多 16 轮）
     *   5. PhysicalPlanner：LogicalPlan → PhysicalPlan（选择具体物理算子）
     *   6. 分派：DDL → UtilityProcessor / SELECT → coroutine generator / DML → Executor
     *
     * SELECT 返回惰性 generator（不立即执行，由调用方逐行消费）。
     * DML（INSERT/UPDATE/DELETE）立即 drain generator 完成写操作。
     */
    ProcessedQuery QueryProcessor::run(const std::string& sql, std::shared_ptr<Session> session) {
        Parser parser;
        Statement stmt = parser.parse(sql);

        // 0) CREATE USER: always available, even before authentication.
        if (auto* cu = std::get_if<CreateUserStmt>(&stmt)) {
            if (session->authenticated && session->auth_user != "admin") {
                throw std::runtime_error("[Auth] Only admin can create users");
            }
            user_manager_.add_user(cu->username, cu->password);
            ProcessedQuery q;
            q.message = "CREATE USER";
            return q;
        }

        // 0b) AUTH: always available, even before authentication.
        if (auto* auth = std::get_if<AuthStmt>(&stmt)) {
            if (!user_manager_.authenticate(auth->username, auth->password)) {
                throw std::runtime_error("[Auth] Authentication failed for user: " + auth->username);
            }
            session->authenticated = true;
            session->auth_user = auth->username;
            ProcessedQuery q;
            q.message = "AUTH OK";
            return q;
        }

        // Require authentication for all other commands (only when users exist).
        if (!session->authenticated && user_manager_.has_users()) {
            throw std::runtime_error("[Auth] Not authenticated. Use AUTH <username> '<password>'");
        }

        // 1) 事务控制语句
        if (auto txn_res = txn_ctrl_.handle(stmt, *session); txn_res.has_value()) {
            ProcessedQuery q;
            q.message = std::move(txn_res->message);
            return q;
        }

        // 1b) CHECKPOINT
        if (std::holds_alternative<CheckpointStmt>(stmt)) {
            storage_.checkpoint();
            ProcessedQuery q;
            q.message = "CHECKPOINT";
            return q;
        }

        // 1c) SHOW STATUS
        if (std::holds_alternative<ShowStatusStmt>(stmt)) {
            ProcessedQuery q;
            q.rows = build_status_rows();
            q.is_select = true;
            return q;
        }

        // 1d) PREPARE: parse and cache the plan.
        if (auto* prep = std::get_if<PrepareStmt>(&stmt)) {
            Parser inner_parser;
            Statement inner_stmt = inner_parser.parse(prep->sql);
            // 非相关 IN (SELECT ...)：PREPARE 时代换（值随计划一并固化，与 PREPARE 的计划冻结语义一致）。
            if (stmt_has_subquery(inner_stmt))
                resolve_subqueries(inner_stmt, session, 0);
            auto plan = build_physical_plan(inner_stmt);
            session->prepared_stmts[prep->name] = std::move(plan);
            ProcessedQuery q;
            q.message = "PREPARE";
            return q;
        }

        // 1e) EXECUTE: run the cached plan.
        if (auto* exec = std::get_if<ExecuteStmt>(&stmt)) {
            auto it = session->prepared_stmts.find(exec->name);
            if (it == session->prepared_stmts.end())
                throw std::runtime_error("[Process] Prepared statement not found: " + exec->name);
            auto shared_plan = it->second; // shared_ptr<PlanNode>

            txn_ctrl_.prepare_for_statement(stmt, *session);
            ExecutionContext ctx{ session, &row_locks_, &catalog_, &storage_, &txn_manager_ };
            if (session->statement_timeout_ms > 0) {
                ctx.deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(session->statement_timeout_ms);
            }
            auto executor = std::make_shared<Executor>(ctx);
            auto gen = executor->run(shared_plan.get());
            ProcessedQuery q;
            q.rows = std::move(gen);
            struct Keeper { std::shared_ptr<PlanNode> plan; std::shared_ptr<Executor> exec; };
            q.plan = std::shared_ptr<void>(std::make_shared<Keeper>(Keeper{ shared_plan, executor }));
            q.is_select = true;
            return q;
        }

        // 1f) DEALLOCATE PREPARE: remove cached plan(s).
        if (auto* dea = std::get_if<DeallocateStmt>(&stmt)) {
            if (dea->name.empty()) {
                session->prepared_stmts.clear();
                ProcessedQuery q;
                q.message = "DEALLOCATE ALL";
                return q;
            }
            session->prepared_stmts.erase(dea->name);
            ProcessedQuery q;
            q.message = "DEALLOCATE PREPARE";
            return q;
        }

        // Failed 状态保护
        if (session->current_txn_id != 0) {
            auto state = txn_manager_.get_state(session->current_txn_id);
            if (state && *state == TxnState::Failed) {
                throw std::runtime_error("[Process] Current transaction is aborted; commands ignored until ROLLBACK");
            }
        }

        // 2) EXPLAIN
        if (std::holds_alternative<std::shared_ptr<ExplainStmt>>(stmt)) {
            const auto& ex = std::get<std::shared_ptr<ExplainStmt>>(stmt);
            // 非相关 IN (SELECT ...)：先代换再规划（EXPLAIN 展示代换后的计划）。
            if (stmt_has_subquery(ex->inner))
                resolve_subqueries(ex->inner, session, 0);
            const auto& inner = ex->inner;
            const bool dml_or_select =
                    std::holds_alternative<SelectStmt>(inner) || std::holds_alternative<InsertStmt>(inner) ||
                    std::holds_alternative<UpdateStmt>(inner) || std::holds_alternative<DeleteStmt>(inner);
            if (dml_or_select) {
                // UNION：各臂独立规划，直接展示物理 UnionPlan（无单一逻辑树文本）。
                if (const auto* usel = std::get_if<SelectStmt>(&inner); usel && !usel->unions.empty()) {
                    std::shared_ptr<PlanNode> shared_plan = build_physical_plan(inner);
                    ProcessedQuery q;
                    q.rows = ExplainPrinter::render(shared_plan.get());
                    q.plan = shared_plan;
                    q.is_select = true;
                    return q;
                }
                opt::LogicalPlanner lplanner{ catalog_ };
                auto lp = lplanner.plan(inner);
                opt::RuleSet rules = opt::make_default_rules();
                lp = rules.apply(std::move(lp));
                std::string ltext = opt::to_string(*lp, 0);

                opt::PhysicalPlanner pplanner{ catalog_, &storage_ };
                std::shared_ptr<PlanNode> shared_plan = pplanner.plan(*lp);

                if (ex->analyze) {
                    // EXPLAIN ANALYZE：带性能分析执行。
                    txn_ctrl_.prepare_for_statement(inner, *session);
                    ExecutionContext ctx{ session, &row_locks_, &catalog_, &storage_, &txn_manager_ };
                    CorrelatedRunner analyze_runner(*this, session);
                    ctx.subquery_runner = &analyze_runner; // 相关子查询支持（行内 drain，栈生命周期安全）
                    if (session->statement_timeout_ms > 0) {
                        ctx.deadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(session->statement_timeout_ms);
                    }
                    auto executor = std::make_shared<Executor>(ctx);
                    QueryStats stats;
                    auto gen = executor->run_profiled(shared_plan.get(), stats);
                    // Drain the generator and discard rows (stats are collected).
                    for (const auto& _: gen) { (void)_; }
                    ProcessedQuery q;
                    q.rows = ExplainPrinter::render_analyze(std::move(ltext), shared_plan.get(), stats);
                    q.plan = shared_plan;
                    q.is_select = true;
                    return q;
                }

                ProcessedQuery q;
                q.rows = ExplainPrinter::render_dual(std::move(ltext), shared_plan.get());
                q.plan = shared_plan;
                q.is_select = true;
                return q;
            }
            std::shared_ptr<PlanNode> shared_plan = build_physical_plan(inner);
            ProcessedQuery q;
            q.rows = ExplainPrinter::render(shared_plan.get());
            q.plan = shared_plan;
            q.is_select = true;
            return q;
        }

        try {
            auto table_names = extract_table_names(stmt);
            const bool ddl_op = is_ddl(stmt);
            const bool is_select = std::holds_alternative<SelectStmt>(stmt);

            // 非相关 IN (SELECT ...)：先执行子查询并代换为字面量 IN 列表（数据相关，跳过计划缓存）。
            const bool had_subquery = stmt_has_subquery(stmt);
            if (had_subquery)
                resolve_subqueries(stmt, session, 0);

            // Invalidate plan cache and prepared statements on DDL.
            if (ddl_op) {
                plan_cache_.invalidate_all();
                session->prepared_stmts.clear();
            }

            std::optional<GlobalLockGuard> global_guard;
            if (ddl_op) {
                global_guard.emplace(lock_manager_, LockMode::Exclusive);
            }

            std::optional<MultiTableLockGuard> table_guard;
            if (!table_names.empty() && !ddl_op && !is_select && !session->in_transaction()) {
                table_guard.emplace(lock_manager_, std::move(table_names), LockMode::Exclusive);
            }

            // Check plan cache for SELECT only (DML has different values per statement).
            std::shared_ptr<PlanNode> shared_plan;
            if (is_select && !had_subquery) {
                std::string normalized = normalize_sql(sql);
                shared_plan = plan_cache_.lookup(normalized);
            }
            if (!shared_plan) {
                auto plan = build_physical_plan(stmt);
                shared_plan = std::move(plan);
                if (is_select && !had_subquery) {
                    plan_cache_.insert(normalize_sql(sql), shared_plan);
                }
            }
            // 3) 分派：DDL → Utility；DML/SELECT → Executor
            if (UtilityProcessor::is_utility_plan(shared_plan.get())) {
                utility_.process(shared_plan.get());
                ProcessedQuery q;
                q.message = "OK";
                q.plan = shared_plan;
                return q;
            }

            // 为 DML/SELECT 准备 snapshot_ts / auto_commit_ts
            txn_ctrl_.prepare_for_statement(stmt, *session);

            ExecutionContext ctx{ session, &row_locks_, &catalog_, &storage_, &txn_manager_ };
            // 相关子查询运行器：执行期逐外层行代换 + 递归执行（随 generator 一同保活）。
            auto subq_runner = std::make_shared<CorrelatedRunner>(*this, session);
            ctx.subquery_runner = subq_runner.get();

            // Set query deadline from session timeout.
            if (session->statement_timeout_ms > 0) {
                ctx.deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(session->statement_timeout_ms);
            }

            if (is_select) {
                // 把 Executor 用 shared_ptr 持有，让其与 generator 一起存活到客户端消费完毕。
                auto executor = std::make_shared<Executor>(ctx);
                auto gen = executor->run(shared_plan.get());
                ProcessedQuery q;
                q.rows = std::move(gen);
                // 用 plan 字段一并保活 plan、executor 与子查询运行器。
                struct Keeper {
                    std::shared_ptr<PlanNode> plan;
                    std::shared_ptr<Executor> exec;
                    std::shared_ptr<SubqueryRunner> runner;
                };
                q.plan = std::shared_ptr<void>(
                        std::make_shared<Keeper>(Keeper{ shared_plan, executor, subq_runner }));
                q.is_select = true;
                return q;
            }

            // 写路径：在此处 drain generator
            Executor executor{ ctx };
            auto gen = executor.run(shared_plan.get());
            for (const auto& _: gen) {
                (void)_;
            }
            ProcessedQuery q;
            q.message = "OK";
            q.plan = shared_plan;
            return q;
        } catch (...) {
            if (session->current_txn_id != 0) {
                txn_manager_.mark_failed(session->current_txn_id);
            }
            throw;
        }
    }

    bool QueryProcessor::subquery_is_correlated(const SelectStmt& sub) {
        if (!sub.where.has_value())
            return false;
        bool found = false;
        detect_outer_refs_bool(*sub.where, subquery_local_names(sub), found);
        return found;
    }

    bool QueryProcessor::bool_has_subquery(const BoolExpr& e) {
        if (e.in_expr.has_value() && e.in_expr->subquery)
            return true;
        if (e.left && bool_has_subquery(*e.left))
            return true;
        return e.right && bool_has_subquery(*e.right);
    }

    bool QueryProcessor::stmt_has_subquery(const Statement& stmt) {
        const std::optional<BoolExpr>* where = nullptr;
        if (const auto* s = std::get_if<SelectStmt>(&stmt))
            where = &s->where;
        else if (const auto* u = std::get_if<UpdateStmt>(&stmt))
            where = &u->where;
        else if (const auto* d = std::get_if<DeleteStmt>(&stmt))
            where = &d->where;
        return where && where->has_value() && bool_has_subquery(**where);
    }

    /**
     * @brief 执行并代换 BoolExpr 树中的非相关 IN (SELECT ...) 子查询。
     *
     * 子查询先于外层规划执行（同会话/同快照），结果代换为字面量 IN 列表——外层因此
     * 可自然命中 IN→IndexScan 优化；空结果遵循 SQL 语义（IN→FALSE，NOT IN→TRUE，含 NULL 三值）。
     */
    void QueryProcessor::resolve_subqueries_in_bool(BoolExpr& e, const std::shared_ptr<Session>& session, int depth) {
        if (depth > 8)
            throw std::runtime_error("[Process] Subquery nesting too deep");
        if (e.left)
            resolve_subqueries_in_bool(*e.left, session, depth);
        if (e.right)
            resolve_subqueries_in_bool(*e.right, session, depth);
        if (!e.in_expr.has_value() || !e.in_expr->subquery)
            return;
        // 相关子查询：保留到执行期逐外层行求值（nested apply），不做规划前代换。
        if (subquery_is_correlated(*e.in_expr->subquery))
            return;
        // 先递归解析子查询自身 WHERE 中的嵌套子查询。
        Statement sub{ SelectStmt(*e.in_expr->subquery) };
        resolve_subqueries(sub, session, depth + 1);
        // 执行子查询，收集单列结果为字面量 IN 列表。
        auto plan = build_physical_plan(sub);
        ExecutionContext ctx{ session, &row_locks_, &catalog_, &storage_, &txn_manager_ };
        CorrelatedRunner pre_runner(*this, session);
        ctx.subquery_runner = &pre_runner; // 支持非相关子查询内部嵌套相关子查询
        if (session->statement_timeout_ms > 0)
            ctx.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(session->statement_timeout_ms);
        Executor ex{ ctx };
        const bool exists_only = e.in_expr->exists_only;
        std::vector<Expression> values;
        bool found = false;
        // 自动提交语句：清除上一条语句遗留的过期 snapshot_ts，子查询读最新已提交；
        // 事务内则沿用事务快照（与外层一致的读视图）。
        const uint64_t saved_snap = session->snapshot_ts;
        if (!session->in_transaction())
            session->snapshot_ts = 0;
        try {
            auto gen = ex.run(plan.get());
            for (auto&& rec: gen) {
                if (exists_only) {
                    found = true;
                    break; // EXISTS 只需存在性，取到首行即止
                }
                if (rec.values.size() != 1)
                    throw std::runtime_error("[Process] IN subquery must return exactly one column");
                values.push_back(Expression{ Literal{ rec.values.front() } });
            }
        } catch (...) {
            session->snapshot_ts = saved_snap;
            throw;
        }
        session->snapshot_ts = saved_snap;
        if (exists_only) {
            // 代换为恒真/恒假比较（NOT EXISTS 由外层 Not 节点组合求值）。
            Comparison c;
            c.op = CompareOp::Eq;
            c.lhs = Literal::from_int(1);
            c.rhs = Literal::from_int(found ? 1 : 0);
            e = BoolExpr::make_comparison(std::move(c));
            return;
        }
        e.in_expr->values = std::move(values);
        e.in_expr->subquery.reset();
    }

    void QueryProcessor::resolve_subqueries(Statement& stmt, const std::shared_ptr<Session>& session, int depth) {
        std::optional<BoolExpr>* where = nullptr;
        if (auto* s = std::get_if<SelectStmt>(&stmt))
            where = &s->where;
        else if (auto* u = std::get_if<UpdateStmt>(&stmt))
            where = &u->where;
        else if (auto* d = std::get_if<DeleteStmt>(&stmt))
            where = &d->where;
        if (where && where->has_value())
            resolve_subqueries_in_bool(**where, session, depth);
    }

} // namespace corodb
