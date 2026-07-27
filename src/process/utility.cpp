// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file utility.cpp
// @brief Utility 处理器实现（DDL：CREATE/DROP TABLE/INDEX）。

#include "corodb/process/utility.h"

#include <stdexcept>

#include "corodb/storage/storage_engine.h"

namespace corodb {

    /**
     * @brief 判断给定计划节点是否为 DDL 计划（CREATE/DROP TABLE/INDEX）。
     */
    bool UtilityProcessor::is_utility_plan(const PlanNode* plan) noexcept {
        if (!plan)
            return false;
        if (dynamic_cast<const CreateTablePlan*>(plan))
            return true;
        if (dynamic_cast<const CreateIndexPlan*>(plan))
            return true;
        if (dynamic_cast<const DropTablePlan*>(plan))
            return true;
        if (dynamic_cast<const DropIndexPlan*>(plan))
            return true;
        return false;
    }

    /**
     * @brief 执行 DDL 命令：按计划节点类型分发到具体处理函数。
     * @param plan DDL 计划节点指针（必须是 utility plan）。
     */
    void UtilityProcessor::process(const PlanNode* plan) {
        if (const auto* p = dynamic_cast<const CreateTablePlan*>(plan)) {
            create_table(*p);
            return;
        }
        if (const auto* p = dynamic_cast<const CreateIndexPlan*>(plan)) {
            create_index(*p);
            return;
        }
        if (const auto* p = dynamic_cast<const DropTablePlan*>(plan)) {
            drop_table(*p);
            return;
        }
        if (const auto* p = dynamic_cast<const DropIndexPlan*>(plan)) {
            drop_index(*p);
            return;
        }
        throw std::runtime_error("[Process] Unsupported utility plan node");
    }

    /**
     * @brief 建表：校验表不存在后向 Catalog 注册新 Table 对象。
     * @param plan 包含表名和列定义的创建计划。
     */
    void UtilityProcessor::create_table(const CreateTablePlan& plan) {
        if (catalog_.lookup(plan.table) || storage_.table_exists(plan.table)) {
            throw std::runtime_error("[Process] Table already exists: " + plan.table);
        }
        auto tbl = std::make_shared<Table>(plan.table, plan.columns, &storage_);
        catalog_.register_table(std::move(tbl));
    }

    /**
     * @brief 在指定表的列上创建索引。
     */
    void UtilityProcessor::create_index(const CreateIndexPlan& plan) {
        if (!plan.table)
            throw std::runtime_error("[Process] CREATE INDEX missing table");
        plan.table->create_index(plan.index_name, plan.columns);
    }

    /**
     * @brief 删表：从 Catalog 注销并调用存储引擎删除持久化文件。
     */
    void UtilityProcessor::drop_table(const DropTablePlan& plan) {
        auto table = catalog_.lookup(plan.table);
        if (!table) {
            if (plan.if_exists)
                return;
            throw std::runtime_error("[Process] Table not found: " + plan.table);
        }
        catalog_.unregister_table(plan.table);
        storage_.drop_table(plan.table);
    }

    /**
     * @brief 删索引：从表中移除指定索引。
     */
    void UtilityProcessor::drop_index(const DropIndexPlan& plan) {
        if (!plan.table) {
            if (plan.if_exists)
                return;
            throw std::runtime_error("[Process] DROP INDEX table not found");
        }
        bool removed = plan.table->drop_index(plan.index_name);
        if (!removed && !plan.if_exists) {
            throw std::runtime_error("[Process] Index not found: " + plan.index_name);
        }
    }

} // namespace corodb
