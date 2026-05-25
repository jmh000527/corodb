// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file logger.h @brief Lightweight structured logger (levels, timestamps). */

#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <print>
#include <string_view>

namespace corodb {

    /** @brief 日志严重级别。 */
    enum class LogLevel : uint8_t {
        Trace = 0, ///< 跟踪
        Debug = 1, ///< 调试
        Info  = 2, ///< 信息
        Warn  = 3, ///< 警告
        Error = 4, ///< 错误
        Off   = 5, ///< 关闭
    };

    /** @brief 线程安全的全系统日志记录器。 */
    class Logger {
    public:
        static Logger& instance();

        void set_level(LogLevel level) noexcept { level_ = level; }
        [[nodiscard]] LogLevel level() const noexcept { return level_; }

        /** @brief 将日志重定向到文件（空路径 = 仅 stderr）。 */
        void set_file(const std::string& path);

        /** @brief 输出一条日志记录。 */
        void log(LogLevel level, std::string_view msg);

    private:
        Logger() = default;

        LogLevel level_{ LogLevel::Info };
        std::mutex mutex_;
        std::ofstream file_;
    };

    /** @brief 返回级别对应的单字符标签。 */
    constexpr char level_char(LogLevel lv) noexcept {
        switch (lv) {
            case LogLevel::Trace: return 'T';
            case LogLevel::Debug: return 'D';
            case LogLevel::Info:  return 'I';
            case LogLevel::Warn:  return 'W';
            case LogLevel::Error: return 'E';
            default:              return '?';
        }
    }

} // namespace corodb

#define LOG_TRACE(fmt, ...)                                                    \
    do {                                                                       \
        if (::corodb::Logger::instance().level() <= ::corodb::LogLevel::Trace) \
            ::corodb::Logger::instance().log(::corodb::LogLevel::Trace,        \
                                              std::format(fmt, ##__VA_ARGS__)); \
    } while (0)

#define LOG_DEBUG(fmt, ...)                                                    \
    do {                                                                       \
        if (::corodb::Logger::instance().level() <= ::corodb::LogLevel::Debug) \
            ::corodb::Logger::instance().log(::corodb::LogLevel::Debug,        \
                                              std::format(fmt, ##__VA_ARGS__)); \
    } while (0)

#define LOG_INFO(fmt, ...)                                                    \
    do {                                                                      \
        if (::corodb::Logger::instance().level() <= ::corodb::LogLevel::Info) \
            ::corodb::Logger::instance().log(::corodb::LogLevel::Info,        \
                                             std::format(fmt, ##__VA_ARGS__)); \
    } while (0)

#define LOG_WARN(fmt, ...)                                                    \
    do {                                                                      \
        if (::corodb::Logger::instance().level() <= ::corodb::LogLevel::Warn) \
            ::corodb::Logger::instance().log(::corodb::LogLevel::Warn,        \
                                             std::format(fmt, ##__VA_ARGS__)); \
    } while (0)

#define LOG_ERROR(fmt, ...)                                                    \
    do {                                                                       \
        if (::corodb::Logger::instance().level() <= ::corodb::LogLevel::Error) \
            ::corodb::Logger::instance().log(::corodb::LogLevel::Error,        \
                                              std::format(fmt, ##__VA_ARGS__)); \
    } while (0)
