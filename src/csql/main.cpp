// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file main.cpp
// @brief CoroDB 命令行 SQL 客户端的入口。

#include "corodb/common/table_renderer.h"
#include "corodb/net/network.h"

#include "corodb/net/port.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <vector>

using corodb::connect_socket;
using corodb::read_response;
using corodb::send_line;

/**
 * @brief 命令行SQL客户端主函数
 *
 * 连接到SQL服务器，执行SQL命令，支持交互式和非交互式模式
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return int 成功返回0，失败返回非零错误码
 */
int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::println(std::cerr, "WSAStartup failed");
        return 1;
    }
#endif

    std::string host = "127.0.0.1";          // 默认连接本地主机
    int port = 4000;                         // 默认连接4000端口
    std::optional<std::string> one_shot_sql; // 一次性SQL命令（非交互式模式）

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
            host = argv[++i]; // 服务器主机名
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]); // 服务器端口
        } else if ((arg == "-e" || arg == "--execute") && i + 1 < argc) {
            // 执行单个SQL命令
            if (one_shot_sql)
                *one_shot_sql += " " + std::string(argv[++i]);
            else
                one_shot_sql = argv[++i];
        } else {
            // 剩余参数作为SQL命令
            if (one_shot_sql)
                *one_shot_sql += " " + arg;
            else
                one_shot_sql = arg;
        }
    }

    // 连接到服务器
    socket_t fd = connect_socket(host, port);
    if (fd == INVALID_SOCKET_VAL) {
        std::println(std::cerr, "Failed to connect to {}:{}", host, port);
        return 1;
    }
    // 客户端使用阻塞 socket；发送与读取超时由网络辅助函数内部处理。
    // read_response 内部使用 poll()/select() 实现超时，不依赖非阻塞 socket。

    // 执行SQL命令的lambda函数
    auto run_sql = [&](const std::string& sql) {
        if (!send_line(fd, sql)) {
            std::println(std::cerr, "Failed to send SQL");
            return false;
        }
        std::string resp = read_response(fd);

        if (resp.starts_with("@TABLE\n")) {
            resp.erase(0, 7);

            std::vector<std::string> headers;
            std::vector<std::vector<std::string>> rows;

            std::istringstream iss(resp);
            std::string line;
            bool first = true;

            auto split_tsv = [](const std::string& s) {
                std::vector<std::string> res;
                std::size_t start = 0;
                while (true) {
                    auto pos = s.find('\t', start);
                    if (pos == std::string::npos) {
                        res.push_back(s.substr(start));
                        break;
                    }
                    res.push_back(s.substr(start, pos - start));
                    start = pos + 1;
                }
                return res;
            };

            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                // Allow empty lines? Data might have empty lines if sanitized?
                // But server sends one row per line.
                if (line.empty() && !first)
                    continue;

                auto parts = split_tsv(line);
                if (first) {
                    headers = std::move(parts);
                    first = false;
                } else {
                    rows.push_back(std::move(parts));
                }
            }

            if (!headers.empty()) {
                corodb::TableRenderOptions opt;
                opt.truncate = false;
                auto right_align = corodb::guess_alignment(rows, headers.size());
                std::print("{}", corodb::render_ascii_table(headers, rows, right_align, opt));
            }
            return true;
        }

        auto start = resp.find_first_not_of("\r\n");
        if (start == std::string::npos) {
            resp.clear();
        } else if (start > 0) {
            resp.erase(0, start);
        }
        std::print("{}", resp);
        return true;
    };

    // 非交互式模式：执行单个SQL命令后退出
    if (one_shot_sql) {
        run_sql(*one_shot_sql);
        close_socket(fd);
        return 0;
    }

    // 交互式模式：持续读取并执行SQL命令
    std::println("Connected to {}:{} (type exit/quit to leave)", host, port);
    std::string line;
    while (true) {
        std::print("sql> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line))
            break;

        if (line == "exit" || line == "quit")
            break; // 退出命令
        if (line.empty())
            continue; // 跳过空行
        if (!run_sql(line))
            break; // 执行SQL命令，失败则退出
    }

    close_socket(fd); // 关闭连接
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
