# C++ DAG Web Orchestrator
# 新网页主入口；旧 A2A 专业 Agent 示例仍作为独立兼容目标保留。

cmake_minimum_required(VERSION 3.15)

# 查找 hiredis (Redis 客户端)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS hiredis)

# Orchestrator 可执行文件
add_executable(ai_orchestrator
    dag_web_main.cpp
    dag_runtime.cpp
)

target_include_directories(ai_orchestrator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a_adapter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_server/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_orchestrator
    CURL::libcurl
    Threads::Threads
)

target_compile_definitions(ai_orchestrator PRIVATE
    AGENT_DAG_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web"
    AGENT_DAG_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

# Math Agent 可执行文件
add_executable(ai_math_agent
    math_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_math_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_math_agent
    a2a
    orchestrator
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

# General Agent 可执行文件（通用问题专用，独立回答，不进行工具调用）
add_executable(ai_general_agent
    general_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_general_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_general_agent
    a2a
    orchestrator
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

# Ops Agent 可执行文件（服务器状态巡检与运维诊断）
add_executable(ai_ops_agent
    ops_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_ops_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_ops_agent
    a2a
    orchestrator
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

# Minutes Agent 可执行文件（读取会议文件并生成 Markdown 纪要）
add_executable(ai_minutes_agent
    minutes_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_minutes_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_minutes_agent
    a2a
    orchestrator
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

# Knowledge Agent 可执行文件（文档向量化入库与知识库问答）
add_executable(ai_knowledge_agent
    knowledge_agent_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/src/examples/redis_task_store.cpp
)

target_include_directories(ai_knowledge_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../orchestrator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_BINARY_DIR}/proto
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(ai_knowledge_agent
    a2a
    orchestrator
    agent_rpc_mcp
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    ${HIREDIS_LIBRARIES}
)

# Registry Server 可执行文件
add_executable(ai_registry_server
    registry_server_main.cpp
)

target_include_directories(ai_registry_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
)

target_link_libraries(ai_registry_server
    a2a
    CURL::libcurl
    Threads::Threads
)

# Interactive Client 可执行文件
add_executable(ai_client
    client_main.cpp
)

target_include_directories(ai_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/include/a2a/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../a2a_adapter/include
    ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(ai_client
    a2a
    a2a_adapter
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    CURL::libcurl
    Threads::Threads
)
