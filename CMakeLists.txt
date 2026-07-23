cmake_minimum_required(VERSION 3.20)
project(DAGMultiAgent VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAG_MULTI_AGENT_BUILD_MCP_SERVER
       "Build the standalone MCP server and its plugins" ON)

include(CTest)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# MCP Client 仍使用 jsoncpp 解析 MCP JSON-RPC 消息。
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

add_subdirectory(common)
add_subdirectory(orchestrator)
add_subdirectory(mcp_client)

if(DAG_MULTI_AGENT_BUILD_MCP_SERVER)
    add_subdirectory(mcp_server ${CMAKE_BINARY_DIR}/mcp_server)
endif()

add_subdirectory(examples)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
