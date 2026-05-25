// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file genconfig.cpp
// @brief 编译期辅助工具：将 Config 默认值写入指定路径。
//
// 仅供 CMake POST_BUILD 调用，用于在 build 目录下生成默认 corodb.conf。
// Usage: corodb_genconfig <output_path>

#include <iostream>
#include <print>
#include <string>

#include "corodb/common/config.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "corodb_genconfig") << " <output_path>\n";
        return 1;
    }
    if (!corodb::Config::write_default_file(argv[1])) {
        std::cerr << "Error: cannot write config file: " << argv[1] << "\n";
        return 1;
    }
    std::println("Wrote default config to {}", argv[1]);
    return 0;
}
