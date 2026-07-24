find_package(rapidcheck REQUIRED)

# 优先使用系统 GTest，避免 Anaconda 中旧版 GTest 与系统头文件 ABI 不一致。
if(EXISTS "/usr/lib/x86_64-linux-gnu/libgtest.a" AND
   EXISTS "/usr/lib/x86_64-linux-gnu/libgtest_main.a")
    add_library(dag_gtest STATIC IMPORTED)
    set_target_properties(dag_gtest PROPERTIES
        IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libgtest.a"
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
    )
    add_library(dag_gtest_main STATIC IMPORTED)
    set_target_properties(dag_gtest_main PROPERTIES
        IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libgtest_main.a"
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
        INTERFACE_LINK_LIBRARIES dag_gtest
    )
    set(DAG_GTEST_LIBRARIES dag_gtest dag_gtest_main)
else()
    find_package(GTest REQUIRED)
    set(DAG_GTEST_LIBRARIES GTest::gtest GTest::gtest_main)
endif()

# 测试桥接头将 RapidCheck 属性测试注册到 GoogleTest。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(test_dag_orchestrator
    test_dag_orchestrator.cpp
    ${CMAKE_SOURCE_DIR}/examples/ai_orchestrator/dag_runtime.cpp
)
target_include_directories(test_dag_orchestrator PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/ai_orchestrator/include
)
target_link_libraries(test_dag_orchestrator PRIVATE
    ${DAG_GTEST_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    agent_rpc_mcp
)
if(TARGET mcp_server)
    target_compile_definitions(test_dag_orchestrator PRIVATE
        DAG_TEST_MCP_SERVER="$<TARGET_FILE:mcp_server>"
        DAG_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:team_design>"
        DAG_TEST_TOOL_CONFIG="${CMAKE_SOURCE_DIR}/config/tool_orchestration.json"
    )
    add_dependencies(test_dag_orchestrator
        mcp_server team_design dag_control workspace_fs web_research
    )
endif()
add_test(NAME DagOrchestratorTest COMMAND test_dag_orchestrator)

add_executable(test_context_memory_manager test_context_memory_manager.cpp)
target_link_libraries(test_context_memory_manager PRIVATE
    context_memory
    ${DAG_GTEST_LIBRARIES}
    Threads::Threads
)
target_compile_definitions(test_context_memory_manager PRIVATE
    DAG_MULTI_AGENT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME ContextMemoryManagerTest COMMAND test_context_memory_manager)

add_executable(test_mcp_integration test_mcp_integration.cpp)
target_link_libraries(test_mcp_integration PRIVATE
    agent_rpc_mcp
    agent_common
    rapidcheck
    ${DAG_GTEST_LIBRARIES}
    Threads::Threads
)
if(TARGET mcp_server)
    target_compile_definitions(test_mcp_integration PRIVATE
        DAG_TEST_MCP_SERVER="$<TARGET_FILE:mcp_server>"
        DAG_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:team_design>"
        DAG_TEST_TOOL_CONFIG="${CMAKE_SOURCE_DIR}/config/tool_orchestration.json"
    )
    add_dependencies(test_mcp_integration
        mcp_server team_design dag_control workspace_fs web_research
    )
endif()
add_test(NAME MCPIntegrationTest COMMAND test_mcp_integration)

add_executable(test_session_manager
    test_session_manager.cpp
    ${CMAKE_SOURCE_DIR}/examples/ai_orchestrator/session_manager.cpp
)
target_include_directories(test_session_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/ai_orchestrator/include
)
target_link_libraries(test_session_manager PRIVATE
    ${DAG_GTEST_LIBRARIES}
    Threads::Threads
)
add_test(NAME SessionManagerTest COMMAND test_session_manager)

if(TARGET mcp_tool_runtime)
    add_library(duplicate_tool_plugin SHARED fixtures/duplicate_tool_plugin.cpp)
    add_library(invalid_schema_plugin SHARED fixtures/invalid_schema_plugin.cpp)
    add_library(duplicate_plugin SHARED fixtures/duplicate_plugin.cpp)
    add_library(duplicate_alias SHARED fixtures/duplicate_plugin.cpp)
    foreach(fixture_target IN ITEMS duplicate_tool_plugin invalid_schema_plugin duplicate_plugin duplicate_alias)
        set_target_properties(${fixture_target} PROPERTIES
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_plugins"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_plugins"
        )
        target_include_directories(${fixture_target} PRIVATE
            ${CMAKE_SOURCE_DIR}/mcp_server/src/interface
        )
    endforeach()
    set_target_properties(duplicate_tool_plugin PROPERTIES OUTPUT_NAME "duplicate_tool")
    set_target_properties(invalid_schema_plugin PROPERTIES OUTPUT_NAME "invalid_schema")
    add_executable(test_tool_orchestration test_tool_orchestration.cpp)
    target_link_libraries(test_tool_orchestration PRIVATE
        mcp_tool_runtime
        ${DAG_GTEST_LIBRARIES}
        Threads::Threads
    )
    target_compile_definitions(test_tool_orchestration PRIVATE
        DAG_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:team_design>"
        DAG_TEST_INVALID_PLUGIN_DIR="$<TARGET_FILE_DIR:duplicate_tool_plugin>"
        DAG_TEST_TOOL_CONFIG="${CMAKE_SOURCE_DIR}/config/tool_orchestration.json"
    )
    add_dependencies(test_tool_orchestration
        team_design dag_control workspace_fs web_research
        duplicate_tool_plugin invalid_schema_plugin duplicate_plugin duplicate_alias
    )
    add_test(NAME ToolOrchestrationTest COMMAND test_tool_orchestration)
endif()
