// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file sql_protocol.h @brief SQL 文本协议格式化（明文端口与 TLS 端口共用）。 */

#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <variant>
#include <utility>

#include "corodb/common/logger.h"
#include "corodb/db/database.h"
#include "corodb/db/session.h"
#include "corodb/executor/executor.h"

namespace corodb::sql_proto {

    /** @brief 执行 SQL 语句并返回格式化的结果字符串（@TABLE/@END 文本协议）。 */
    inline std::string run_sql(Database& db, const std::string& sql, std::shared_ptr<Session> session) {
        std::string res;
        const std::string indent = "  ";

        auto result = db.execute(sql, std::move(session));

        if (result.message) {
            std::istringstream iss(*result.message);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) {
                    res += indent;
                    res += line;
                    res += '\n';
                }
            }
        } else if (result.rows) {
            if (result.is_select) {
                res += "@TABLE\n";

                auto value_to_string = [](const Value& v) {
                    if (std::holds_alternative<NullValue>(v))
                        return std::string("NULL");
                    if (std::holds_alternative<int64_t>(v))
                        return std::to_string(std::get<int64_t>(v));
                    if (std::holds_alternative<double>(v))
                        return std::to_string(std::get<double>(v));
                    return std::get<std::string>(v);
                };

                auto sanitize = [](std::string s) {
                    for (char& ch: s) {
                        if (ch == '\t' || ch == '\n' || ch == '\r')
                            ch = ' ';
                    }
                    return s;
                };

                bool have_header = false;

                for (const auto& rec: *result.rows) {
                    if (!have_header) {
                        for (std::size_t i = 0; i < rec.bindings.size(); ++i) {
                            if (i > 0)
                                res += '\t';
                            const auto& b = rec.bindings[i];
                            std::string name = b.column.empty() ? ("col" + std::to_string(i + 1)) : b.column;
                            res += sanitize(name);
                        }
                        res += '\n';
                        have_header = true;
                    }

                    for (std::size_t i = 0; i < rec.values.size(); ++i) {
                        if (i > 0)
                            res += '\t';
                        res += sanitize(value_to_string(rec.values[i]));
                    }
                    res += '\n';
                }
            } else {
                for (const auto& _: *result.rows) {
                    (void)_;
                }
                res += indent;
                res += "OK\n";
            }
        }

        auto start = res.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            res.erase(0, start);
        } else {
            res.clear();
        }

        if (!res.empty()) {
            if (!res.starts_with("@TABLE")) {
                res.insert(0, indent);
            }
        } else {
            res = indent + "OK\n";
        }

        res += "@END\n";
        return res;
    }

    /** @brief 处理单行 SQL（trim + 去分号 + 执行 + 错误包装）。 */
    inline std::string process_sql_line(Database& db, std::string line, std::shared_ptr<Session> session) {
        auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
        auto start = std::find_if_not(line.begin(), line.end(), is_ws);
        auto end = std::find_if_not(line.rbegin(), std::make_reverse_iterator(start), is_ws).base();

        if (start != line.begin() || end != line.end()) {
            line = std::string(start, end);
        }

        // Remove trailing semicolon
        if (!line.empty() && line.back() == ';') {
            line.pop_back();
            start = std::find_if_not(line.begin(), line.end(), is_ws);
            end = std::find_if_not(line.rbegin(), std::make_reverse_iterator(start), is_ws).base();
            if (start != line.begin() || end != line.end()) {
                line = std::string(start, end);
            }
        }

        if (line.empty()) {
            return "";
        }

        try {
            // Database 内部已实现线程安全（读写锁）
            return run_sql(db, line, std::move(session));
        } catch (const WriteConflictError& ex) {
            // 写写冲突是事务并发的预期行为，不打印到 stderr
            return std::string("ERROR: ") + ex.what() + "\n@END\n";
        } catch (const std::exception& ex) {
            LOG_ERROR("Error executing query: {}", ex.what());
            // 保持 "ERROR:" 前缀（大写），客户端必须用 is_error_response() 判定（B2）。
            // 必须追加 @END 终止标记，否则客户端 read_response 会等满 30s 超时
            return std::string("ERROR: ") + ex.what() + "\n@END\n";
        }
    }

} // namespace corodb::sql_proto
