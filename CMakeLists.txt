cmake_minimum_required(VERSION 3.26)
project(corodb
    VERSION 0.1.0
    DESCRIPTION "C++23 coroutine-driven relational database engine"
    LANGUAGES CXX
)

# ============================================================================
# 全局设置
# ============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 统一输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# 默认构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# 源文件
# ============================================================================
add_library(corodb
    # SQL & 执行
    src/sql/parser.cpp
    src/executor/executor.cpp
    src/executor/expression_evaluator.cpp
    src/executor/bool_evaluator.cpp
    src/process/utility.cpp
    src/process/explain_printer.cpp
    src/process/transaction_controller.cpp
    src/process/query_processor.cpp

    # 优化器
    src/plan/logical_plan.cpp
    src/optimizer/logical/logical_planner.cpp
    src/optimizer/logical/rule.cpp
    src/optimizer/logical/constant_folding_rule.cpp
    src/optimizer/logical/predicate_pushdown_rule.cpp
    src/optimizer/logical/column_pruning_rule.cpp
    src/optimizer/logical/projection_merge_rule.cpp
    src/optimizer/logical/join_reorder_rule.cpp
    src/optimizer/physical/physical_planner.cpp

    # 存储引擎
    src/storage/storage_engine_base.cpp
    src/storage/storage_engine_common.cpp
    src/storage/table.cpp
    src/storage/buffer_pool.cpp
    src/storage/lsm_storage_engine.cpp

    # 事务
    src/txn/transaction_manager.cpp
    src/txn/row_lock_manager.cpp
    src/txn/lock_manager.cpp

    # 网络
    src/threading/thread_pool.cpp
    src/threading/event_loop.cpp
    src/threading/connection.cpp
    src/threading/acceptor.cpp
    src/threading/reactor_server.cpp
    src/net/network.cpp

    # 公共
    src/common/types.cpp
    src/common/config.cpp
    src/common/logger.cpp
    src/common/table_renderer.cpp

    # 服务
    src/server/server.cpp
    src/db/database.cpp
)

target_include_directories(corodb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 编译器选项（必须在 target 创建之后）
if(MSVC)
    target_compile_options(corodb PRIVATE /W4 /WX)
    target_compile_definitions(corodb PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(corodb PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# 平台链接
if(WIN32)
    target_link_libraries(corodb PUBLIC Ws2_32)
elseif(UNIX)
    target_link_libraries(corodb PUBLIC pthread)
endif()

# ============================================================================
# 可执行文件
# ============================================================================

# 服务器
add_executable(corodb_server src/server/main.cpp)
target_link_libraries(corodb_server PRIVATE corodb)

# 配置生成器
add_executable(corodb_genconfig src/server/genconfig.cpp)
target_link_libraries(corodb_genconfig PRIVATE corodb)

# 编译后生成默认配置文件（已有则不覆盖）
add_custom_command(TARGET corodb_server POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DEXE=$<TARGET_FILE:corodb_genconfig>
        -DCONF_PATH=$<TARGET_FILE_DIR:corodb_server>/corodb.conf
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_default_config.cmake
    VERBATIM
)
add_dependencies(corodb_server corodb_genconfig)

# CLI 客户端
add_executable(csql src/csql/main.cpp)
target_link_libraries(csql PRIVATE corodb)

# ============================================================================
# Google Test（优先本地缓存，网络不可用时使用源码）
# ============================================================================
include(FetchContent)

set(GTEST_DIR "${CMAKE_BINARY_DIR}/_deps/googletest-src")

# 若本地已有 googletest 源码则直接使用（跳过网络下载）
if(EXISTS "${GTEST_DIR}/CMakeLists.txt")
    message(STATUS "Using cached Google Test at ${GTEST_DIR}")
    FetchContent_Declare(googletest SOURCE_DIR "${GTEST_DIR}")
else()
    message(STATUS "Downloading Google Test v1.14.0...")
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
endif()

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# 辅助函数：创建模块测试
function(add_module_test MODULE_NAME)
    set(TARGET_NAME "test_${MODULE_NAME}")
    add_executable(${TARGET_NAME}
        tests/test_main.cpp
        tests/test_${MODULE_NAME}.cpp
    )
    target_link_libraries(${TARGET_NAME}
        PRIVATE corodb GTest::gtest GTest::gtest_main GTest::gmock
    )
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /wd4834)
    endif()
    gtest_discover_tests(${TARGET_NAME})
endfunction()

add_module_test(types)
add_module_test(parser)
add_module_test(storage)
add_module_test(execution)
add_module_test(database)

# 全量测试
add_executable(corodb_tests
    tests/test_main.cpp
    tests/test_types.cpp
    tests/test_parser.cpp
    tests/test_storage.cpp
    tests/test_execution.cpp
    tests/test_database.cpp
    tests/test_transaction.cpp
    tests/test_optimizer.cpp
    tests/test_config.cpp
)
target_link_libraries(corodb_tests
    PRIVATE corodb GTest::gtest GTest::gtest_main GTest::gmock
)
target_include_directories(corodb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(corodb_tests PRIVATE /wd4834)
endif()
gtest_discover_tests(corodb_tests)

# ============================================================================
# 并发基准测试
# ============================================================================
add_executable(bench_concurrency
    tests/bench_concurrency.cpp
    tests/bench_utils.cpp
)
target_link_libraries(bench_concurrency PRIVATE corodb)
target_include_directories(bench_concurrency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/include
)
if(MSVC)
    target_compile_options(bench_concurrency PRIVATE /wd4834)
endif()

# ============================================================================
# 安装规则
# ============================================================================
install(TARGETS corodb_server csql corodb_genconfig
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/bin/corodb.conf
    DESTINATION bin
    OPTIONAL
)
