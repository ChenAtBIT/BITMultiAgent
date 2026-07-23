add_subdirectory(ai_orchestrator)

# MCP/RAG 独立示例保留，后续可接入动态 DAG Agent。
add_executable(rag_mcp_example rag_mcp_example.cpp)
target_link_libraries(rag_mcp_example PRIVATE
    agent_rpc_mcp
    agent_common
)
