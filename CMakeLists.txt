cmake_minimum_required(VERSION 3.15)
project(AgentCommunicationRPC VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# 启用测试
enable_testing()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

if(EXISTS "/usr/lib/x86_64-linux-gnu/libgtest.a" AND EXISTS "/usr/lib/x86_64-linux-gnu/libgtest_main.a")
    if(NOT TARGET GTest::gtest)
        add_library(GTest::gtest STATIC IMPORTED GLOBAL)
        set_target_properties(GTest::gtest PROPERTIES
            IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libgtest.a"
            INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
        )
    endif()

    if(NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main STATIC IMPORTED GLOBAL)
        set_target_properties(GTest::gtest_main PROPERTIES
            IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libgtest_main.a"
            INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
            INTERFACE_LINK_LIBRARIES "GTest::gtest"
        )
    endif()
else()
    find_package(GTest REQUIRED)
endif()

# 包含目录
include_directories(${GRPC_INCLUDE_DIRS})
include_directories(${PROTOBUF_INCLUDE_DIRS})
include_directories(${JSONCPP_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/proto/generated)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/common/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/registry/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/server/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/client/include)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # protobuf 生成代码里存在 DEBUG 枚举值，避免与全局 DEBUG 宏冲突。
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
endif()

# 查找CURL依赖 (A2A模块需要)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# A2A模块包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/a2a/include)

# A2A适配层包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/a2a_adapter/include)

# Orchestrator模块包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/orchestrator/include)

# 添加子目录
add_subdirectory(proto)
add_subdirectory(common)
add_subdirectory(registry)
add_subdirectory(a2a)
add_subdirectory(a2a_adapter)
add_subdirectory(orchestrator)
# MCP模块 - 用于 AI Agent 工具集成
add_subdirectory(mcp)
# add_subdirectory(ai_interface)
add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(examples)
add_subdirectory(tests)
