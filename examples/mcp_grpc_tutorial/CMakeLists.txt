cmake_minimum_required(VERSION 3.15)

add_executable(mcp_tutorial_direct
    mcp_tutorial_direct.cpp
)

target_include_directories(mcp_tutorial_direct PRIVATE
    ${CMAKE_SOURCE_DIR}/mcp/include
    ${CMAKE_SOURCE_DIR}/common/include
)

target_link_libraries(mcp_tutorial_direct PRIVATE
    agent_rpc_mcp
    agent_rpc_common
    jsoncpp
    pthread
)

add_executable(grpc_mcp_tutorial_server
    grpc_mcp_tutorial_server.cpp
)

target_include_directories(grpc_mcp_tutorial_server PRIVATE
    ${CMAKE_SOURCE_DIR}/mcp/include
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(grpc_mcp_tutorial_server PRIVATE
    agent_rpc_mcp
    agent_rpc_common
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    jsoncpp
    pthread
)

add_executable(grpc_mcp_tutorial_client
    grpc_mcp_tutorial_client.cpp
)

target_include_directories(grpc_mcp_tutorial_client PRIVATE
    ${CMAKE_SOURCE_DIR}/proto
    ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(grpc_mcp_tutorial_client PRIVATE
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    pthread
)
