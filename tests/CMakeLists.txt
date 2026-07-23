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
)
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
add_test(NAME MCPIntegrationTest COMMAND test_mcp_integration)

add_executable(test_rag_mcp_properties test_rag_mcp_properties.cpp)
target_link_libraries(test_rag_mcp_properties PRIVATE
    agent_rpc_mcp
    agent_common
    rapidcheck
    ${DAG_GTEST_LIBRARIES}
    CURL::libcurl
    Threads::Threads
)
add_test(NAME RAGMCPPropertiesTest COMMAND test_rag_mcp_properties)
