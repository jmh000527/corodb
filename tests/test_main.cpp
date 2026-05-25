/**
 * @file test_main.cpp
 * @brief 单元测试入口文件
 * @author CoroDB Team
 * @date 2026-01-10
 *
 * 这是 CoroDB 单元测试的主入口文件。
 * 使用 Google Test 框架进行测试。
 */

#include <gtest/gtest.h>

// 如果需要自定义测试入口，可以在这里添加
// 默认情况下，使用 gtest_main 链接库即可

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
