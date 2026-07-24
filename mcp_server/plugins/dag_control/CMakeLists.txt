add_library(dag_control SHARED DagControl.cpp)
set_target_properties(dag_control PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dag_control"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
)
target_include_directories(dag_control PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
