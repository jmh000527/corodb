// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file audit.h @brief 审计日志（P2）：把每条语句的执行事件落盘为 JSON 行。
 *
 * 生产运维要求：谁（user/role）在何时对什么（SQL）做了什么、结果如何。
 * 每行一个 JSON 对象（JSON Lines），按 [audit].path 追加写入，线程安全。 */

#pragma once

#include <mutex>
#include <string>

namespace corodb {

    class AuditLogger {
    public:
        static AuditLogger& instance();

        struct Event {
            std::string user;      ///< 认证用户名（未认证为空）
            std::string role;      ///< 角色：admin/read_write/read_only/anonymous
            std::string sql;       ///< 语句文本（超长截断）
            std::string status;    ///< ok | error | denied
            std::string error;     ///< 异常消息（status != ok 时）
            double duration_ms{ 0.0 };
        };

        /** @brief 记录一条审计事件（按 [audit] 配置决定是否落盘与路径）。 */
        void log(const Event& event);

    private:
        AuditLogger() = default;
        std::mutex mutex_;
    };

} // namespace corodb
