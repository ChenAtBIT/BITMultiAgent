add_library(workspace_fs SHARED WorkspaceFs.cpp)
set_target_properties(workspace_fs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "workspace_fs"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
)
target_include_directories(workspace_fs PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
