// Copyright (c) 2024 CoroDB Authors. All rights reserved.

#include "corodb/common/logger.h"

#include <chrono>

namespace corodb {

    /** @brief 返回 Logger 全局单例（Mayer's pattern，线程安全）。 */
    Logger& Logger::instance() {
        static Logger logger;
        return logger;
    }

    /** @brief 将日志输出重定向到文件（空路径 = 仅 stderr）。 */
    void Logger::set_file(const std::string& path) {
        std::lock_guard lock(mutex_);
        if (file_.is_open())
            file_.close();
        if (!path.empty()) {
            file_.open(path, std::ios::app);
        }
    }

    /** @brief 输出一条日志记录（本地时间戳 + 级别 + 消息）。 */
    void Logger::log(LogLevel level, std::string_view msg) {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto local = std::chrono::zoned_time{ std::chrono::current_zone(), now };
        auto name = [](LogLevel lv) {
            switch (lv) {
                case LogLevel::Trace: return "TRACE";
                case LogLevel::Debug: return "DEBUG";
                case LogLevel::Info:  return "INFO";
                case LogLevel::Warn:  return "WARN";
                case LogLevel::Error: return "ERROR";
                default:              return "?";
            }
        };
        std::string line = std::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}",
                                       local,
                                       name(level),
                                       msg);

        std::lock_guard lock(mutex_);
        std::println(std::cerr, "{}", line);
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
    }

} // namespace corodb
