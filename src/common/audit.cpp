// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file audit.cpp
// @brief 审计日志实现（JSON Lines 追加写入）。

#include "corodb/common/audit.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#include "corodb/common/config.h"

namespace corodb {

    namespace {

        std::string iso8601_now() {
            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            char out[40];
            std::snprintf(out, sizeof(out), "%s.%03lldZ", buf, static_cast<long long>(ms.count()));
            return out;
        }

        /** @brief JSON 字符串转义（控制字符/引号/反斜杠）。 */
        std::string json_escape(const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (unsigned char c: in) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                }
            }
            return out;
        }

    } // namespace

    AuditLogger& AuditLogger::instance() {
        static AuditLogger logger;
        return logger;
    }

    void AuditLogger::log(const Event& event) {
        const Config& cfg = Config::instance();
        if (!cfg.audit_enabled())
            return;

        std::string sql = event.sql;
        if (sql.size() > 512)
            sql = sql.substr(0, 512) + "...";

        std::ostringstream line;
        line << "{\"ts\":\"" << iso8601_now() << "\"";
        line << ",\"user\":\"" << json_escape(event.user) << "\"";
        line << ",\"role\":\"" << json_escape(event.role) << "\"";
        line << ",\"sql\":\"" << json_escape(sql) << "\"";
        line << ",\"status\":\"" << json_escape(event.status) << "\"";
        if (!event.error.empty())
            line << ",\"error\":\"" << json_escape(event.error) << "\"";
        char dur[32];
        std::snprintf(dur, sizeof(dur), "%.3f", event.duration_ms);
        line << ",\"duration_ms\":" << dur << "}\n";

        // 审计文件按行追加；写入失败静默丢弃（审计不可用不应阻断业务，告警交给日志层）。
        std::lock_guard lk(mutex_);
        std::ofstream ofs(cfg.audit_path(), std::ios::app);
        if (!ofs)
            return;
        ofs << line.str();
    }

} // namespace corodb
