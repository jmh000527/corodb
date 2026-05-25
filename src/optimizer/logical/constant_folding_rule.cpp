// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file constant_folding_rule.cpp
// @brief 常量折叠优化规则（R3）实现。

#include "corodb/optimizer/logical/constant_folding_rule.h"

namespace corodb::opt {

    namespace detail {

        /** @brief 若表达式为字面量则提取其值，否则返回 nullopt。 */
        std::optional<Value> as_literal(const Expression& e) {
            if (auto* lit = std::get_if<Literal>(&e))
                return lit->value;
            return std::nullopt;
        }

        /** @brief 对两个已知字面量进行二元运算折叠，返回结果值。 */
        std::optional<Value> fold_binary(BinaryExpr::Op op, const Value& l, const Value& r) {
            if (std::holds_alternative<NullValue>(l) || std::holds_alternative<NullValue>(r))
                return Value{ NullValue{} };
            if (op == BinaryExpr::Op::Concat) {
                if (std::holds_alternative<std::string>(l) && std::holds_alternative<std::string>(r))
                    return Value{ std::get<std::string>(l) + std::get<std::string>(r) };
                return std::nullopt;
            }
            if (!std::holds_alternative<int64_t>(l) || !std::holds_alternative<int64_t>(r))
                return std::nullopt;
            int64_t a = std::get<int64_t>(l);
            int64_t b = std::get<int64_t>(r);
            switch (op) {
                case BinaryExpr::Op::Add:
                    return Value{ a + b };
                case BinaryExpr::Op::Sub:
                    return Value{ a - b };
                case BinaryExpr::Op::Mul:
                    return Value{ a * b };
                case BinaryExpr::Op::Div:
                    if (b == 0)
                        return std::nullopt;
                    return Value{ a / b };
                case BinaryExpr::Op::Mod:
                    if (b == 0)
                        return std::nullopt;
                    return Value{ a % b };
                default:
                    return std::nullopt;
            }
        }

        // 递归折叠表达式中的常量子树
        Expression fold_expression(Expression expr) {
            if (auto bin_ptr = std::get_if<std::shared_ptr<BinaryExpr>>(&expr)) {
                if (!*bin_ptr)
                    return expr;
                BinaryExpr folded;
                folded.op = (*bin_ptr)->op;
                folded.lhs = fold_expression((*bin_ptr)->lhs);
                folded.rhs = fold_expression((*bin_ptr)->rhs);
                auto lv = as_literal(folded.lhs);
                auto rv = as_literal(folded.rhs);
                if (lv && rv) {
                    if (auto v = fold_binary(folded.op, *lv, *rv))
                        return Literal{ *v };
                }
                return std::make_shared<BinaryExpr>(std::move(folded));
            }
            return expr;
        }

        // 递归折叠布尔表达式中的常量子树
        BoolExpr fold_bool(BoolExpr expr) {
            switch (expr.kind) {
                case BoolExpr::Kind::Comparison:
                    if (expr.cmp.has_value()) {
                        expr.cmp->lhs = fold_expression(std::move(expr.cmp->lhs));
                        expr.cmp->rhs = fold_expression(std::move(expr.cmp->rhs));
                    }
                    break;
                case BoolExpr::Kind::And:
                case BoolExpr::Kind::Or:
                    if (expr.left)
                        *expr.left = fold_bool(std::move(*expr.left));
                    if (expr.right)
                        *expr.right = fold_bool(std::move(*expr.right));
                    break;
                case BoolExpr::Kind::Not:
                    if (expr.left)
                        *expr.left = fold_bool(std::move(*expr.left));
                    break;
                case BoolExpr::Kind::In:
                case BoolExpr::Kind::Between:
                    break;
            }
            return expr;
        }

    } // namespace detail

    std::string ConstantFoldingRule::name() const {
        return "R3.ConstantFolding";
    }

    /**
     * @brief 对外入口：触发常量折叠改写。
     */
    RuleResult ConstantFoldingRule::apply(LogicalPlanPtr plan) {
        bool changed = false;
        rewrite(plan, changed);
        return RuleResult{ std::move(plan), changed };
    }

    /**
     * @brief 递归遍历计划树，对每个节点内的表达式和谓词执行常量折叠。
     */
    void ConstantFoldingRule::rewrite(LogicalPlanPtr& plan, bool& changed) {
        if (!plan)
            return;
        std::visit([&](auto& n) { rewrite_node(n, changed); }, plan->node);
    }

    /**
     * @brief 将表达式序列化为可比较的字符串（用于改写前后的等价判断）。
     */
    std::string ConstantFoldingRule::serialize_expr(const Expression& e) {
        if (auto* l = std::get_if<Literal>(&e))
            return "L:" + value_str(l->value);
        if (auto* c = std::get_if<ColumnRef>(&e))
            return "C:" + c->table + "." + c->name;
        if (auto* b = std::get_if<std::shared_ptr<BinaryExpr>>(&e)) {
            if (!*b)
                return "B:null";
            return "B(" + std::string(BinaryExpr::op_str((*b)->op)) + "," + serialize_expr((*b)->lhs) + "," +
                   serialize_expr((*b)->rhs) + ")";
        }
        return "?";
    }

    /**
     * @brief 将布尔表达式序列化为可比较的字符串。
     */
    std::string ConstantFoldingRule::serialize(const BoolExpr& b) {
        std::string s = "k" + std::to_string(static_cast<int>(b.kind));
        if (b.cmp.has_value())
            s += "[" + serialize_expr(b.cmp->lhs) + " " + compare_op_str(b.cmp->op) + " " + serialize_expr(b.cmp->rhs) +
                 "]";
        if (b.left)
            s += "L{" + serialize(*b.left) + "}";
        if (b.right)
            s += "R{" + serialize(*b.right) + "}";
        return s;
    }

    /**
     * @brief 将 Value 转换为可读字符串（用于序列化与调试）。
     */
    std::string ConstantFoldingRule::value_str(const Value& v) {
        if (std::holds_alternative<int64_t>(v))
            return std::to_string(std::get<int64_t>(v));
        if (std::holds_alternative<std::string>(v))
            return "'" + std::get<std::string>(v) + "'";
        return "NULL";
    }

} // namespace corodb::opt
