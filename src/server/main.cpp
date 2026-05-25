// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file main.cpp
// @brief CoroDB 数据库服务器主程序入口。
//
// 本程序不接受任何命令行参数：所有可调参数集中在 Config 类与配置文件中。
// 启动顺序：
//   1. 读取 corodb_server 可执行文件所在目录的 `corodb.conf`；
//   2. 若不存在，则尝试当前工作目录下的 `corodb.conf`；
//   3. 仍不存在则使用内置默认值。
// 该配置文件由 CMake 在编译完成后自动写入到构建目录，用户可手动编辑。

#include <filesystem>
#include <iostream>
#include <print>
#include <string>

#include "corodb/common/config.h"
#include "corodb/server/server.h"

using corodb::Config;
using corodb::run_server;
using corodb::ServerConfig;

namespace {

    std::string locate_config(const char* argv0) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path exe_dir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
        if (!ec && !exe_dir.empty()) {
            fs::path candidate = exe_dir / "corodb.conf";
            if (fs::exists(candidate))
                return candidate.string();
        }
        if (fs::exists("corodb.conf"))
            return "corodb.conf";
        return {};
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        std::cerr << "corodb_server: command-line arguments are not supported.\n"
                  << "Edit corodb.conf in the binary directory to change configuration.\n";
        return 1;
    }

    auto& config = Config::instance();
    const std::string config_path = locate_config(argv[0]);
    if (!config_path.empty()) {
        std::string err;
        if (!config.load_from_file(config_path, &err)) {
            std::cerr << "Warning: failed to load config '" << config_path << "': " << err << " (using defaults)\n";
        } else {
            std::println("Loaded config from {}", config_path);
        }
    } else {
        // 未找到配置文件，自动生成默认配置到可执行文件同目录
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path exe_dir = fs::weakly_canonical(fs::path(argv[0]), ec).parent_path();
        fs::path default_path = (!ec && !exe_dir.empty()) ? exe_dir / "corodb.conf" : fs::path("corodb.conf");
        if (Config::write_default_file(default_path.string())) {
            std::println("Generated default config at {}", default_path.string());
            config.load_from_file(default_path.string());
        } else {
            std::println("No corodb.conf found; using built-in defaults.");
        }
    }

    if (config.data_dir().empty()) {
        std::cerr << "Error: server.data_dir cannot be empty\n";
        return 1;
    }

    ServerConfig cfg;
    cfg.port = config.server_port();
    cfg.data_dir = config.data_dir();
    cfg.max_connections = config.max_connections();
    cfg.io_threads = config.io_threads();
    cfg.worker_threads = config.worker_threads();
    cfg.reuse_port = config.reuse_port();
    cfg.idle_timeout_sec = config.idle_timeout_sec();
    cfg.statement_timeout_ms = config.statement_timeout_ms();
    cfg.thread_pool_max_queue = config.thread_pool_max_queue();

    return run_server(cfg);
}
