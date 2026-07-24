add_library(team_design SHARED TeamDesign.cpp)
set_target_properties(team_design PROPERTIES
    PREFIX ""
    OUTPUT_NAME "team_design"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
)
target_include_directories(team_design PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
