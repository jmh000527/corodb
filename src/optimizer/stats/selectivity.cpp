// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file selectivity.cpp
// @brief 选择率估计器实现（T1.5）。

#include "corodb/optimizer/stats/selectivity.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace corodb::opt {

    namespace {

        [[nodiscard]] double clamp01(double v) noexcept {
            return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        }

        /** @brief 统计是否可用于估计。 */
        [[nodiscard]] bool usable(const ColumnStats* s) noexcept {
            return s && s->valid() && s->total_rows > 0;
        }

        /** @brief MCV 频率总和。 */
        [[nodiscard]] double mcv_total(const ColumnStats& s) noexcept {
            double sum = 0.0;
            for (double f: s.mcv_freqs)
                sum += f;
            return sum;
        }

        /** @brief v 若为 MCV 值返回其频率，否则 0。 */
        [[nodiscard]] double mcv_freq_of(const ColumnStats& s, const Value& v) noexcept {
            ValueEq eq;
            for (std::size_t i = 0; i < s.mcv_values.size() && i < s.mcv_freqs.size(); ++i) {
                if (eq(s.mcv_values[i], v))
                    return s.mcv_freqs[i];
            }
            return 0.0;
        }

        /** @brief 数值视图（int64/double → double），非数值返回 nullopt。 */
        [[nodiscard]] std::optional<double> to_num(const Value& v) noexcept {
            if (const auto* i = std::get_if<int64_t>(&v))
                return static_cast<double>(*i);
            if (const auto* d = std::get_if<double>(&v))
                return *d;
            return std::nullopt;
        }

        /**
         * @brief v 在等高直方图中的位置（0..1 的人口分位）。
         *
         * bounds 为 n_buckets+1 个真实数据值；bucket i 覆盖 [i/n, (i+1)/n)。
         * 边界桶内：双方均数值时线性插值，否则取桶中点。v 超出首尾界 → 0 或 1。
         */
        [[nodiscard]] double histogram_position(const std::vector<Value>& bounds, const Value& v) noexcept {
            const std::size_t last = bounds.size() - 1; // bounds[last] = max
            ValueLess lt;
            if (lt(v, bounds[0]))
                return 0.0;
            if (!lt(v, bounds[last]))
                return 1.0;
            // 找最大 i 使 bounds[i] <= v（v 落入 bucket i）
            std::size_t lo = 0, hi = last; // 不变量：bounds[lo] <= v < bounds[hi]
            while (hi - lo > 1) {
                std::size_t mid = (lo + hi) / 2;
                if (lt(v, bounds[mid]))
                    hi = mid;
                else
                    lo = mid;
            }
            const std::size_t nbuckets = last;
            double frac = 0.5; // 非数值类型（字符串等）：桶内取中点
            if (auto a = to_num(bounds[lo]); a) {
                if (auto b = to_num(bounds[hi]); b) {
                    if (auto x = to_num(v); x && *b > *a)
                        frac = clamp01((*x - *a) / (*b - *a));
                }
            }
            return (static_cast<double>(lo) + frac) / static_cast<double>(nbuckets);
        }

        /** @brief 提取比较叶的 (列, 操作符, 字面量)，支持 col OP lit 与 lit OP col。 */
        struct ColOpLit {
            const ColumnRef* col;
            CompareOp op;
            const Literal* lit;
        };

        [[nodiscard]] std::optional<ColOpLit> extract_col_op_lit(const Comparison& cmp) {
            CompareOp op = cmp.op;
            if (auto* c = std::get_if<ColumnRef>(&cmp.lhs)) {
                if (auto* l = std::get_if<Literal>(&cmp.rhs))
                    return ColOpLit{ c, op, l };
                return std::nullopt;
            }
            if (auto* c = std::get_if<ColumnRef>(&cmp.rhs)) {
                if (auto* l = std::get_if<Literal>(&cmp.lhs)) {
                    // lit OP col：反转方向（lit < col ≡ col > lit）
                    switch (op) {
                        case CompareOp::Lt: op = CompareOp::Gt; break;
                        case CompareOp::Le: op = CompareOp::Ge; break;
                        case CompareOp::Gt: op = CompareOp::Lt; break;
                        case CompareOp::Ge: op = CompareOp::Le; break;
                        default: break; // Eq/Ne 对称
                    }
                    return ColOpLit{ c, op, l };
                }
            }
            return std::nullopt;
        }

    } // anonymous namespace

    // =========================================================================
    // 单谓词
    // =========================================================================

    double SelectivityEstimator::selectivity_eq(const ColumnStats* s, const Value& value) {
        if (!usable(s) || is_null(value))
            return kDefaultEqSelectivity;
        // MCV 命中：精确频率。
        if (double f = mcv_freq_of(*s, value); f > 0.0)
            return clamp01(f);
        // 未命中：非 MCV、非 NULL 行均摊到其余 NDV。
        const double mcv_sum = mcv_total(*s);
        const double other_rows = 1.0 - s->null_frac - mcv_sum;
        const std::size_t mcv_count = s->mcv_values.size();
        if (s->ndistinct == 0)
            return kDefaultEqSelectivity;
        double denom = static_cast<double>(s->ndistinct);
        if (s->ndistinct > mcv_count)
            denom = static_cast<double>(s->ndistinct - mcv_count);
        double sel = other_rows > 0.0 ? other_rows / denom : kDefaultEqSelectivity;
        // 下限：估计行数至少 1 行。
        sel = std::max(sel, 1.0 / static_cast<double>(s->total_rows));
        return clamp01(sel);
    }

    double SelectivityEstimator::selectivity_ne(const ColumnStats* s, const Value& value) {
        if (!usable(s) || is_null(value))
            return 1.0 - kDefaultEqSelectivity;
        return clamp01(1.0 - selectivity_eq(s, value));
    }

    double SelectivityEstimator::selectivity_range(const ColumnStats* s, const std::optional<Value>& low,
                                                   bool low_inclusive, const std::optional<Value>& high,
                                                   bool high_inclusive) {
        if (!usable(s))
            return kDefaultRangeSelectivity;

        ValueLess lt;
        ValueEq eq;

        // 1) MCV 贡献：落在 [low, high]（按 inclusive）内的 MCV 频率求和。
        double mcv_part = 0.0;
        for (std::size_t i = 0; i < s->mcv_values.size() && i < s->mcv_freqs.size(); ++i) {
            const Value& v = s->mcv_values[i];
            const bool ge_low = !low.has_value() || lt(*low, v) || (low_inclusive && eq(*low, v));
            const bool le_high = !high.has_value() || lt(v, *high) || (high_inclusive && eq(v, *high));
            if (ge_low && le_high)
                mcv_part += s->mcv_freqs[i];
        }

        // 2) 直方图贡献：直方图人口占比 × 范围分位差。
        const double hist_pop = clamp01(1.0 - s->null_frac - mcv_total(*s));
        if (s->histogram_bounds.size() < 2 || hist_pop <= 0.0)
            return clamp01(mcv_part + hist_pop * kDefaultRangeSelectivity);

        double pos_low = 0.0;
        if (low.has_value()) {
            pos_low = histogram_position(s->histogram_bounds, *low);
            if (!low_inclusive)
                pos_low -= mcv_freq_of(*s, *low); // 开界：剔除恰等于下界的 MCV 行
        }
        double pos_high = 1.0;
        if (high.has_value()) {
            pos_high = histogram_position(s->histogram_bounds, *high);
            if (!high_inclusive)
                pos_high -= mcv_freq_of(*s, *high);
        }

        const double hist_frac = clamp01(pos_high) - clamp01(pos_low);
        return clamp01(mcv_part + hist_pop * clamp01(hist_frac));
    }

    double SelectivityEstimator::selectivity_between(const ColumnStats* s, const Value& low, const Value& high) {
        return selectivity_range(s, std::optional<Value>(low), true, std::optional<Value>(high), true);
    }

    double SelectivityEstimator::selectivity_in_list(const ColumnStats* s, const std::vector<Value>& values) {
        if (values.empty())
            return 0.0;
        // 去重后求和，封顶 1。
        std::unordered_set<Value, ValueHash, ValueEq> seen;
        double sum = 0.0;
        for (const auto& v: values) {
            if (seen.insert(v).second)
                sum += selectivity_eq(s, v);
        }
        return clamp01(sum);
    }

    double SelectivityEstimator::selectivity_is_null(const ColumnStats* s) {
        return usable(s) ? clamp01(s->null_frac) : 0.0;
    }

    // =========================================================================
    // 组合
    // =========================================================================

    double SelectivityEstimator::combine_and(double a, double b) noexcept {
        return clamp01(a * b);
    }

    double SelectivityEstimator::combine_or(double a, double b) noexcept {
        return clamp01(a + b - a * b);
    }

    // =========================================================================
    // 上层入口：整棵谓词树
    // =========================================================================

    double SelectivityEstimator::estimate_filter_selectivity(const Table& table, const BoolExpr& predicate) {
        switch (predicate.kind) {
            case BoolExpr::Kind::And: {
                double l = predicate.left ? estimate_filter_selectivity(table, *predicate.left) : 1.0;
                double r = predicate.right ? estimate_filter_selectivity(table, *predicate.right) : 1.0;
                return combine_and(l, r);
            }
            case BoolExpr::Kind::Or: {
                double l = predicate.left ? estimate_filter_selectivity(table, *predicate.left) : 0.0;
                double r = predicate.right ? estimate_filter_selectivity(table, *predicate.right) : 0.0;
                return combine_or(l, r);
            }
            case BoolExpr::Kind::Not: {
                double s = predicate.left ? estimate_filter_selectivity(table, *predicate.left)
                                          : kDefaultRangeSelectivity;
                return clamp01(1.0 - s);
            }
            case BoolExpr::Kind::Comparison: {
                if (!predicate.cmp.has_value())
                    return kDefaultRangeSelectivity;
                auto leaf = extract_col_op_lit(*predicate.cmp);
                if (!leaf)
                    return kDefaultRangeSelectivity; // col OP col 等复杂叶
                // 表限定不符（引用其它表）→ 无信息。
                if (!leaf->col->table.empty() && leaf->col->table != table.name())
                    return kDefaultRangeSelectivity;
                const ColumnStats* cs = table.column_stats(leaf->col->name);
                if (!usable(cs))
                    return leaf->op == CompareOp::Eq ? kDefaultEqSelectivity : kDefaultRangeSelectivity;
                switch (leaf->op) {
                    case CompareOp::Eq:
                        return selectivity_eq(cs, leaf->lit->value);
                    case CompareOp::Ne:
                        return selectivity_ne(cs, leaf->lit->value);
                    case CompareOp::Lt:
                        return selectivity_range(cs, std::nullopt, false,
                                                 std::optional<Value>(leaf->lit->value), false);
                    case CompareOp::Le:
                        return selectivity_range(cs, std::nullopt, false,
                                                 std::optional<Value>(leaf->lit->value), true);
                    case CompareOp::Gt:
                        return selectivity_range(cs, std::optional<Value>(leaf->lit->value), false,
                                                 std::nullopt, false);
                    case CompareOp::Ge:
                        return selectivity_range(cs, std::optional<Value>(leaf->lit->value), true,
                                                 std::nullopt, false);
                    default:
                        return kDefaultRangeSelectivity; // Like 等
                }
            }
            case BoolExpr::Kind::Between: {
                if (!predicate.between_expr.has_value())
                    return kDefaultRangeSelectivity;
                const auto& be = *predicate.between_expr;
                auto* col = std::get_if<ColumnRef>(&be.expr);
                auto* lo = std::get_if<Literal>(&be.low);
                auto* hi = std::get_if<Literal>(&be.high);
                if (!col || !lo || !hi)
                    return kDefaultRangeSelectivity;
                if (!col->table.empty() && col->table != table.name())
                    return kDefaultRangeSelectivity;
                double s = selectivity_between(table.column_stats(col->name), lo->value, hi->value);
                return be.negated ? clamp01(1.0 - s) : s;
            }
            case BoolExpr::Kind::In: {
                if (!predicate.in_expr.has_value())
                    return kDefaultRangeSelectivity;
                const auto& ie = *predicate.in_expr;
                auto* col = std::get_if<ColumnRef>(&ie.expr);
                if (!col || ie.values.empty() || ie.subquery)
                    return kDefaultRangeSelectivity;
                if (!col->table.empty() && col->table != table.name())
                    return kDefaultRangeSelectivity;
                std::vector<Value> vals;
                vals.reserve(ie.values.size());
                for (const auto& e: ie.values) {
                    if (const auto* lit = std::get_if<Literal>(&e))
                        vals.push_back(lit->value);
                }
                if (vals.empty())
                    return kDefaultRangeSelectivity;
                double s = selectivity_in_list(table.column_stats(col->name), vals);
                return ie.negated ? clamp01(1.0 - s) : s;
            }
            default:
                return kDefaultRangeSelectivity;
        }
    }

} // namespace corodb::opt
