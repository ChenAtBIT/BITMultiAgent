add_library(agent_common STATIC
    src/logger.cpp
)

target_include_directories(agent_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(agent_common PUBLIC
    Threads::Threads
)

target_compile_options(agent_common PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
