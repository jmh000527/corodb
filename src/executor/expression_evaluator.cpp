// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file expression_evaluator.cpp
 *  @brief ExpressionEvaluator 实现（标量表达式求值）。
 */

#include "corodb/executor/expression_evaluator.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace corodb {

    /**
     * @brief 按列引用（表名 + 列名）从 record 中查找对应值。
     * @param ref 列引用，table 为空时仅按列名匹配。
     * @return 对应值的常量引用。
     */
    const Value& ExpressionEvaluator::lookup(const Record& record, const ColumnRef& ref) {
        for (std::size_t i = 0; i < record.bindings.size(); ++i) {
            const auto& b = record.bindings[i];
            if ((ref.table.empty() || b.table == ref.table) && b.column == ref.name) {
                return record.values[i];
            }
        }
        throw std::runtime_error("[Executor] Column not found: " +
                                 (ref.table.empty() ? ref.name : ref.table + "." + ref.name));
    }

    /**
     * @brief 按列名从 record 中查找对应值（不区分表名）。
     * @param name 列名。
     */
    const Value& ExpressionEvaluator::lookup_by_name(const Record& record, const std::string& name) {
        for (std::size_t i = 0; i < record.bindings.size(); ++i) {
            if (record.bindings[i].column == name)
                return record.values[i];
        }
        throw std::runtime_error("[Executor] Column not found: " + name);
    }

    /**
     * @brief 对标量函数调用求值（UPPER / LOWER / LENGTH / TRIM / SUBSTR / ABS / COALESCE / NULLIF）。
     * @param func 函数表达式节点。
     * @return 函数计算结果值。
     */
    Value ExpressionEvaluator::eval_function(const Record& record, const FunctionExpr& func) {
        switch (func.func) {
            case ScalarFunc::Coalesce: {
                for (const auto& arg: func.args) {
                    Value v = eval(record, arg);
                    if (!std::holds_alternative<NullValue>(v))
                        return v;
                }
                return Value{ NullValue{} };
            }
            case ScalarFunc::Nullif: {
                Value a = eval(record, func.args[0]);
                Value b = eval(record, func.args[1]);
                if (a == b)
                    return Value{ NullValue{} };
                return a;
            }
            case ScalarFunc::Upper: {
                Value v = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(v))
                    return v;
                if (!std::holds_alternative<std::string>(v))
                    throw std::runtime_error("[Executor] UPPER requires string argument");
                std::string s = std::get<std::string>(v);
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                return Value{ std::move(s) };
            }
            case ScalarFunc::Lower: {
                Value v = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(v))
                    return v;
                if (!std::holds_alternative<std::string>(v))
                    throw std::runtime_error("[Executor] LOWER requires string argument");
                std::string s = std::get<std::string>(v);
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return Value{ std::move(s) };
            }
            case ScalarFunc::Length: {
                Value v = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(v))
                    return v;
                if (!std::holds_alternative<std::string>(v))
                    throw std::runtime_error("[Executor] LENGTH requires string argument");
                return Value{ static_cast<int64_t>(std::get<std::string>(v).length()) };
            }
            case ScalarFunc::Trim: {
                Value v = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(v))
                    return v;
                if (!std::holds_alternative<std::string>(v))
                    throw std::runtime_error("[Executor] TRIM requires string argument");
                std::string s = std::get<std::string>(v);
                auto start = s.find_first_not_of(" \t\n\r");
                if (start == std::string::npos)
                    return Value{ std::string{} };
                auto end = s.find_last_not_of(" \t\n\r");
                return Value{ s.substr(start, end - start + 1) };
            }
            case ScalarFunc::Substr: {
                Value str_val = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(str_val))
                    return str_val;
                if (!std::holds_alternative<std::string>(str_val))
                    throw std::runtime_error("[Executor] SUBSTR requires string as first argument");
                const std::string& s = std::get<std::string>(str_val);
                Value start_val = eval(record, func.args[1]);
                if (!std::holds_alternative<int64_t>(start_val))
                    throw std::runtime_error("[Executor] SUBSTR requires integer start position");
                int64_t start = std::get<int64_t>(start_val);
                if (start < 1)
                    start = 1;
                std::size_t pos = static_cast<std::size_t>(start - 1);
                if (pos >= s.length())
                    return Value{ std::string{} };
                std::size_t len = s.length() - pos;
                if (func.args.size() > 2) {
                    Value len_val = eval(record, func.args[2]);
                    if (!std::holds_alternative<int64_t>(len_val))
                        throw std::runtime_error("[Executor] SUBSTR requires integer length");
                    int64_t l = std::get<int64_t>(len_val);
                    if (l < 0)
                        l = 0;
                    len = static_cast<std::size_t>(l);
                }
                return Value{ s.substr(pos, len) };
            }
            case ScalarFunc::Abs: {
                Value v = eval(record, func.args[0]);
                if (std::holds_alternative<NullValue>(v))
                    return v;
                if (!std::holds_alternative<int64_t>(v))
                    throw std::runtime_error("[Executor] ABS requires integer argument");
                int64_t n = std::get<int64_t>(v);
                return Value{ n < 0 ? -n : n };
            }
            default:
                throw std::runtime_error("[Executor] Unknown scalar function");
        }
    }

    /**
     * @brief 对表达式树求值（列引用 / 字面量 / 二元算术 / 标量函数 / 聚合引用）。
     * @param expr 待求值表达式。
     * @return 计算得到的值。
     */
    Value ExpressionEvaluator::eval(const Record& record, const Expression& expr) {
        return std::visit(
                [&](const auto& node) -> Value {
                    using T = std::decay_t<decltype(node)>;

                    // 列引用：从 Record 绑定中查找对应值
                    if constexpr (std::is_same_v<T, ColumnRef>) {
                        return lookup(record, node);
                        // 字面量：直接返回常量值
                    } else if constexpr (std::is_same_v<T, Literal>) {
                        return node.value;
                        // 二元算术：+、-、*、/、%、||（字符串拼接）
                    } else if constexpr (std::is_same_v<T, std::shared_ptr<BinaryExpr>>) {
                        if (!node)
                            throw std::runtime_error("[Executor] Invalid binary expression");
                        Value lhs = eval(record, node->lhs);
                        Value rhs = eval(record, node->rhs);

                        // || 拼接：NULL 视为空串（SQL NULL-safe concat）
                        if (node->op == BinaryExpr::Op::Concat) {
                            std::string l_str, r_str;
                            if (!std::holds_alternative<NullValue>(lhs)) {
                                if (std::holds_alternative<std::string>(lhs))
                                    l_str = std::get<std::string>(lhs);
                                else if (std::holds_alternative<int64_t>(lhs))
                                    l_str = std::to_string(std::get<int64_t>(lhs));
                                else if (std::holds_alternative<double>(lhs))
                                    l_str = std::to_string(std::get<double>(lhs));
                            }
                            if (!std::holds_alternative<NullValue>(rhs)) {
                                if (std::holds_alternative<std::string>(rhs))
                                    r_str = std::get<std::string>(rhs);
                                else if (std::holds_alternative<int64_t>(rhs))
                                    r_str = std::to_string(std::get<int64_t>(rhs));
                                else if (std::holds_alternative<double>(rhs))
                                    r_str = std::to_string(std::get<double>(rhs));
                            }
                            return Value{ l_str + r_str };
                        }

                        if (std::holds_alternative<NullValue>(lhs) || std::holds_alternative<NullValue>(rhs))
                            return Value{ NullValue{} };

                        // 检测操作数是否为 Float64，是则提升为 double 运算。
                        const bool has_float =
                                std::holds_alternative<double>(lhs) || std::holds_alternative<double>(rhs);

                        if (!has_float) {
                            if (!std::holds_alternative<int64_t>(lhs) || !std::holds_alternative<int64_t>(rhs))
                                throw std::runtime_error("[Executor] Unsupported operand type for arithmetic");

                            int64_t l = std::get<int64_t>(lhs);
                            int64_t r = std::get<int64_t>(rhs);

                            switch (node->op) {
                                case BinaryExpr::Op::Add:
                                    return Value{ static_cast<int64_t>(l + r) };
                                case BinaryExpr::Op::Sub:
                                    return Value{ static_cast<int64_t>(l - r) };
                                case BinaryExpr::Op::Mul:
                                    return Value{ static_cast<int64_t>(l * r) };
                                case BinaryExpr::Op::Div:
                                    if (r == 0)
                                        throw std::runtime_error("[Executor] Division by zero");
                                    return Value{ static_cast<double>(l) / static_cast<double>(r) };
                                case BinaryExpr::Op::Mod:
                                    if (r == 0)
                                        throw std::runtime_error("[Executor] Modulo by zero");
                                    return Value{ static_cast<int64_t>(l % r) };
                                case BinaryExpr::Op::Concat:
                                    break;
                            }
                        } else {
                            auto to_double = [](const Value& v) -> double {
                                if (std::holds_alternative<int64_t>(v))
                                    return static_cast<double>(std::get<int64_t>(v));
                                if (std::holds_alternative<double>(v))
                                    return std::get<double>(v);
                                throw std::runtime_error("[Executor] Non-numeric operand in float arithmetic");
                            };
                            double l = to_double(lhs);
                            double r = to_double(rhs);

                            switch (node->op) {
                                case BinaryExpr::Op::Add:
                                    return Value{ l + r };
                                case BinaryExpr::Op::Sub:
                                    return Value{ l - r };
                                case BinaryExpr::Op::Mul:
                                    return Value{ l * r };
                                case BinaryExpr::Op::Div:
                                    if (r == 0.0)
                                        throw std::runtime_error("[Executor] Division by zero");
                                    return Value{ l / r };
                                case BinaryExpr::Op::Mod:
                                    throw std::runtime_error("[Executor] Modulo not supported for float operands");
                                case BinaryExpr::Op::Concat:
                                    break;
                            }
                        }
                        throw std::runtime_error("[Executor] Unknown binary operator");
                        // 标量函数：COALESCE / UPPER / LOWER / LENGTH / SUBSTR / ABS 等
                    } else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionExpr>>) {
                        if (!node)
                            throw std::runtime_error("[Executor] Invalid function expression");
                        return eval_function(record, *node);
                    } else if constexpr (std::is_same_v<T, AggregateExpr>) {
                        // 从 Record 中查找已计算的聚合结果。
                        // 先按参数列名匹配，再兜底取最后一个值。
                        if (node.arg.has_value()) {
                            for (std::size_t i = 0; i < record.bindings.size(); ++i) {
                                if (record.bindings[i].column == node.arg->name)
                                    return record.values[i];
                            }
                        }
                        if (!record.values.empty())
                            return record.values.back();
                        throw std::runtime_error("[Executor] Aggregate not found in record: " + node.to_string());
                    } else {
                        return Value{ NullValue{} };
                    }
                },
                expr);
    }

} // namespace corodb
