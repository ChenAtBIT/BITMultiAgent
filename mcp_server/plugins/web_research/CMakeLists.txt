find_package(CURL REQUIRED)
find_package(Iconv)

add_library(web_research SHARED WebResearch.cpp)
set_target_properties(web_research PROPERTIES
    PREFIX ""
    OUTPUT_NAME "web_research"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
)
target_include_directories(web_research PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
target_link_libraries(web_research PRIVATE CURL::libcurl)
if(Iconv_FOUND)
    target_link_libraries(web_research PRIVATE Iconv::Iconv)
    target_compile_definitions(web_research PRIVATE WEB_RESEARCH_HAS_ICONV=1)
endif()
