// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/**
 * @file join_reorder_rule.h
 * @brief Join 连接顺序重排优化规则（R5）。
 *
 * ## 规则说明
 *
 * 对多个 Inner Join 进行启发式重排，将行数估算较少的表放到 Join 树的左侧
 * （驱动侧），利用 Hash Join 左侧构建哈希表的特性，减小哈希表大小，
 * 从而降低内存占用并提升 Probe 阶段的缓存命中率。
 *
 * 当前采用的启发式策略：
 * 1. 收集连续 Inner Join 链（左深树）的所有叶子节点（基表 Scan / 子计划）。
 * 2. 按行数估算（TableStats.row_count）升序排列（小表优先）。
 * 3. 重新构建左深 Join 树：最小表放最左，依次向右。
 * 4. ON 条件（等值谓词）从原连接中收集后重新分配到合适位置。
 *
 * 行数估算来源（优先级从高到低）：
 * - `TableStats.row_count`（存储层统计信息，如有）
 * - `ScanNode.estimated_rows`（规划时填充的估算值）
 * - 默认值 `1000`（统计信息缺失时保守估算）
 *
 * 不适用场景：
 * - 含 LEFT/RIGHT/FULL OUTER JOIN 的连接（语义约束顺序，不可重排）。
 * - 连接链中只有一个 Join（无需重排）。
 *
 * ## SQL 示例
 *
 * **示例 1 — 小表前置**
 * ```sql
 * SELECT e.name, d.name, l.city
 *   FROM employees e         -- 100,000 行
 *   JOIN departments d       -- 50 行
 *     ON e.dept_id = d.id
 *   JOIN locations l         -- 200 行
 *     ON d.location_id = l.id;
 * ```
 * 重排前（按 FROM 顺序，左深树）：
 * ```
 * Join(e.dept_id = d.id)
 *   ├─ Join(d.location_id = l.id)
 *   │    ├─ Scan(employees, ~100000 rows)
 *   │    └─ Scan(locations,    ~200 rows)
 *   └─ Scan(departments,        ~50 rows)
 * ```
 * 重排后（小表 departments 最左，locations 次之）：
 * ```
 * Join(e.dept_id = d.id)
 *   ├─ Join(d.location_id = l.id)
 *   │    ├─ Scan(departments, ~50 rows)   ← 最小，先做 Hash Build
 *   │    └─ Scan(locations,  ~200 rows)
 *   └─ Scan(employees, ~100000 rows)
 * ```
 *
 * **示例 2 — 统计信息缺失时的保守策略**
 * ```sql
 * SELECT * FROM a JOIN b ON a.id = b.a_id JOIN c ON b.id = c.b_id;
 * ```
 * 若三张表均无统计信息，均估算为 1000 行，规则不改变顺序（稳定排序）。
 *
 * **示例 3 — OUTER JOIN 跳过**
 * ```sql
 * SELECT e.name, d.name
 *   FROM employees e
 *   LEFT JOIN departments d ON e.dept_id = d.id;
 * ```
 * LEFT JOIN 语义要求保留左表所有行，规则跳过，不做任何重排。
 */

#pragma once

#include "corodb/optimizer/logical/rule.h"

namespace corodb::opt {

    /**
     * @brief 启发式重排 Inner Join 顺序，将小表置于驱动侧（规则 R5）。
     *
     * 收集连续 Inner Join 链的所有叶子节点，按行数估算升序重建左深 Join 树，
     * 使 Hash Join 构建阶段使用最小的哈希表。
     * 对含 OUTER JOIN 的节点跳过，不做重排。
     */
    class JoinReorderRule final : public Rule {
    public:
        std::string name() const override;

        RuleResult apply(LogicalPlanPtr plan) override;

    private:
        LogicalPlanPtr rewrite(LogicalPlanPtr plan, bool& changed);

        struct JoinLeaf {
            LogicalPlanPtr node;
            int64_t estimated_rows{ 1000 };
        };

        /// 递归展开连续 Inner Join 链，收集叶子节点和 ON 条件。
        bool collect_inner_joins(LogicalPlanPtr plan, std::vector<JoinLeaf>& leaves, std::vector<BoolExpr>& conditions);

        /// 按行数估算重建左深 Inner Join 树。
        LogicalPlanPtr rebuild_join_tree(std::vector<JoinLeaf>& leaves, std::vector<BoolExpr>& conditions);

        static int64_t estimate_rows(const LogicalPlan& plan);
    };

} // namespace corodb::opt
