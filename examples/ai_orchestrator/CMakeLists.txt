add_executable(ai_orchestrator
    dag_web_main.cpp
    dag_runtime.cpp
    session_manager.cpp
)

target_include_directories(ai_orchestrator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/mcp_server/include
)

target_link_libraries(ai_orchestrator PRIVATE
    CURL::libcurl
    Threads::Threads
    agent_rpc_mcp
)

target_compile_definitions(ai_orchestrator PRIVATE
    AGENT_DAG_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web"
    AGENT_DAG_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    AGENT_DAG_BUILD_ROOT="${CMAKE_BINARY_DIR}"
    AGENT_DAG_TOOL_CONFIG="${CMAKE_SOURCE_DIR}/config/tool_orchestration.json"
)

target_compile_options(ai_orchestrator PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
