add_library(context_memory STATIC
    src/context_memory_manager.cpp
)

target_include_directories(context_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(context_memory PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
