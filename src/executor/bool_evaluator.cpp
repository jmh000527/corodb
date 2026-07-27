// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file bool_evaluator.cpp
 *  @brief BoolEvaluator 实现（布尔/比较表达式三值逻辑）。
 */

#include "corodb/executor/bool_evaluator.h"

#include <stdexcept>

#include "corodb/executor/expression_evaluator.h"

namespace corodb {

    /**
     * @brief 对两个值进行三值比较（NULL 视为最大值）。
     * @return -1 / 0 / 1，分别表示 lhs < / = / > rhs。
     */
    int BoolEvaluator::compare_order(const Value& lhs, const Value& rhs) {
        if (std::holds_alternative<NullValue>(lhs) && std::holds_alternative<NullValue>(rhs))
            return 0;
        if (std::holds_alternative<NullValue>(lhs))
            return 1;
        if (std::holds_alternative<NullValue>(rhs))
            return -1;

        // 允许 int64 与 double 比较（int64 提升为 double）。
        const bool l_num = std::holds_alternative<int64_t>(lhs) || std::holds_alternative<double>(lhs);
        const bool r_num = std::holds_alternative<int64_t>(rhs) || std::holds_alternative<double>(rhs);

        if (l_num && r_num) {
            auto to_double = [](const Value& v) -> double {
                if (std::holds_alternative<int64_t>(v)) return static_cast<double>(std::get<int64_t>(v));
                return std::get<double>(v);
            };
            double l = to_double(lhs);
            double r = to_double(rhs);
            if (l < r) return -1;
            if (l > r) return 1;
            return 0;
        }

        if (lhs.index() != rhs.index())
            throw std::runtime_error("[Executor] Type mismatch in comparison");

        if (std::holds_alternative<int64_t>(lhs)) {
            auto l = std::get<int64_t>(lhs);
            auto r = std::get<int64_t>(rhs);
            return (l < r) ? -1 : (l > r ? 1 : 0);
        }
        // 注：double-double 比较已在上面 l_num && r_num 分支处理。
        const auto& l = std::get<std::string>(lhs);
        const auto& r = std::get<std::string>(rhs);
        if (l < r) return -1;
        if (l > r) return 1;
        return 0;
    }

    /**
     * @brief 对单个比较谓词（包含 IS NULL / LIKE 等）求值，返回三值逻辑结果。
     * @param cmp 比较谓词。
     * @param record 当前行上下文。
     */
    SqlBool BoolEvaluator::eval_comparison(const Comparison& cmp, const Record& record) {
        Value lhs = ExpressionEvaluator::eval(record, cmp.lhs);

        if (cmp.op == CompareOp::IsNull)
            return std::holds_alternative<NullValue>(lhs) ? SqlBool::True : SqlBool::False;
        if (cmp.op == CompareOp::IsNotNull)
            return std::holds_alternative<NullValue>(lhs) ? SqlBool::False : SqlBool::True;

        Value rhs = ExpressionEvaluator::eval(record, cmp.rhs);

        if (cmp.op == CompareOp::Like || cmp.op == CompareOp::NotLike) {
            if (std::holds_alternative<NullValue>(lhs) || std::holds_alternative<NullValue>(rhs))
                return SqlBool::Unknown;
            if (!std::holds_alternative<std::string>(lhs) || !std::holds_alternative<std::string>(rhs))
                return SqlBool::False;
            const std::string& text = std::get<std::string>(lhs);
            const std::string& pattern = std::get<std::string>(rhs);
            auto match_like = [](const std::string& t, const std::string& p) -> bool {
                std::size_t ti = 0, pi = 0;
                std::size_t star_pi = std::string::npos, star_ti = 0;
                while (ti < t.size()) {
                    if (pi < p.size() && (p[pi] == '_' || p[pi] == t[ti])) {
                        ++ti;
                        ++pi;
                    } else if (pi < p.size() && p[pi] == '%') {
                        star_pi = pi++;
                        star_ti = ti;
                    } else if (star_pi != std::string::npos) {
                        pi = star_pi + 1;
                        ti = ++star_ti;
                    } else {
                        return false;
                    }
                }
                while (pi < p.size() && p[pi] == '%')
                    ++pi;
                return pi == p.size();
            };
            bool matched = match_like(text, pattern);
            if (cmp.op == CompareOp::NotLike)
                return matched ? SqlBool::False : SqlBool::True;
            return matched ? SqlBool::True : SqlBool::False;
        }

        if (std::holds_alternative<NullValue>(lhs) || std::holds_alternative<NullValue>(rhs))
            return SqlBool::Unknown;

        int ord = compare_order(lhs, rhs);
        bool result = false;
        switch (cmp.op) {
            case CompareOp::Eq:
                result = (ord == 0);
                break;
            case CompareOp::Ne:
                result = (ord != 0);
                break;
            case CompareOp::Lt:
                result = (ord < 0);
                break;
            case CompareOp::Le:
                result = (ord <= 0);
                break;
            case CompareOp::Gt:
                result = (ord > 0);
                break;
            case CompareOp::Ge:
                result = (ord >= 0);
                break;
            default:
                break;
        }
        return result ? SqlBool::True : SqlBool::False;
    }

    /**
     * @brief 对整棵布尔表达式树（AND / OR / NOT / IN / BETWEEN）求值。
     * @param expr 布尔表达式根节点。
     * @param record 当前行上下文。
     * @return True / False / Unknown 三值结果。
     */
    SqlBool BoolEvaluator::eval(const BoolExpr& expr, const Record& record) {
        auto and3 = [](SqlBool a, SqlBool b) -> SqlBool {
            if (a == SqlBool::False || b == SqlBool::False)
                return SqlBool::False;
            if (a == SqlBool::Unknown || b == SqlBool::Unknown)
                return SqlBool::Unknown;
            return SqlBool::True;
        };
        auto or3 = [](SqlBool a, SqlBool b) -> SqlBool {
            if (a == SqlBool::True || b == SqlBool::True)
                return SqlBool::True;
            if (a == SqlBool::Unknown || b == SqlBool::Unknown)
                return SqlBool::Unknown;
            return SqlBool::False;
        };
        auto not3 = [](SqlBool a) -> SqlBool {
            if (a == SqlBool::True)
                return SqlBool::False;
            if (a == SqlBool::False)
                return SqlBool::True;
            return SqlBool::Unknown;
        };

        switch (expr.kind) {
            case BoolExpr::Kind::Comparison:
                if (!expr.cmp.has_value())
                    throw std::runtime_error("[Executor] Missing comparison expression");
                return eval_comparison(*expr.cmp, record);
            case BoolExpr::Kind::And:
                if (!expr.left || !expr.right)
                    throw std::runtime_error("[Executor] Malformed AND expression");
                return and3(eval(*expr.left, record), eval(*expr.right, record));
            case BoolExpr::Kind::Or:
                if (!expr.left || !expr.right)
                    throw std::runtime_error("[Executor] Malformed OR expression");
                return or3(eval(*expr.left, record), eval(*expr.right, record));
            case BoolExpr::Kind::Not:
                if (!expr.left)
                    throw std::runtime_error("[Executor] Malformed NOT expression");
                return not3(eval(*expr.left, record));
            case BoolExpr::Kind::In: {
                if (!expr.in_expr.has_value())
                    throw std::runtime_error("[Executor] Missing IN expression");
                const auto& in = *expr.in_expr;
                if (in.subquery)
                    throw std::runtime_error("[Executor] Unresolved IN subquery (must be resolved before execution)");
                Value lhs = ExpressionEvaluator::eval(record, in.expr);
                if (std::holds_alternative<NullValue>(lhs))
                    return SqlBool::Unknown;
                bool found = false;
                bool has_null = false;
                for (const auto& val: in.values) {
                    Value rhs = ExpressionEvaluator::eval(record, val);
                    if (std::holds_alternative<NullValue>(rhs)) {
                        has_null = true;
                        continue;
                    }
                    if (lhs == rhs) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    return in.negated ? SqlBool::False : SqlBool::True;
                if (has_null)
                    return SqlBool::Unknown;
                return in.negated ? SqlBool::True : SqlBool::False;
            }
            case BoolExpr::Kind::Between: {
                if (!expr.between_expr.has_value())
                    throw std::runtime_error("[Executor] Missing BETWEEN expression");
                const auto& between = *expr.between_expr;
                Value val = ExpressionEvaluator::eval(record, between.expr);
                Value low = ExpressionEvaluator::eval(record, between.low);
                Value high = ExpressionEvaluator::eval(record, between.high);
                if (std::holds_alternative<NullValue>(val) || std::holds_alternative<NullValue>(low) ||
                    std::holds_alternative<NullValue>(high))
                    return SqlBool::Unknown;
                bool in_range = (compare_order(val, low) >= 0) && (compare_order(val, high) <= 0);
                if (between.negated)
                    return in_range ? SqlBool::False : SqlBool::True;
                return in_range ? SqlBool::True : SqlBool::False;
            }
        }
        return SqlBool::Unknown;
    }

} // namespace corodb
