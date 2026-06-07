cmake_minimum_required(VERSION 3.10)

if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libgcc")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    link_libraries(pthread)
endif()

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

set(SQLITE_VEC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite-vec")
file(READ "${SQLITE_VEC_SOURCE_DIR}/VERSION" SQLITE_VEC_VERSION_RAW)
string(STRIP "${SQLITE_VEC_VERSION_RAW}" VERSION)
string(REPLACE "." ";" SQLITE_VEC_VERSION_PARTS "${VERSION}")
list(GET SQLITE_VEC_VERSION_PARTS 0 VERSION_MAJOR)
list(GET SQLITE_VEC_VERSION_PARTS 1 VERSION_MINOR)
list(GET SQLITE_VEC_VERSION_PARTS 2 SQLITE_VEC_PATCH_RAW)
string(REGEX REPLACE "-.*$" "" VERSION_PATCH "${SQLITE_VEC_PATCH_RAW}")
string(TIMESTAMP DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
set(SOURCE "vendored")

set(SQLITE_VEC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite_vec_generated")
file(MAKE_DIRECTORY "${SQLITE_VEC_GENERATED_DIR}")
configure_file(
    "${SQLITE_VEC_SOURCE_DIR}/sqlite-vec.h.tmpl"
    "${SQLITE_VEC_GENERATED_DIR}/sqlite-vec.h"
)

add_library(knowledge_base_tools SHARED
    ${PROJECT_SOURCE_DIR}/plugins/knowledge_base_tools/KnowledgeBaseTools.cpp
    ${SQLITE_VEC_SOURCE_DIR}/sqlite-vec.c
)

set_source_files_properties(
    ${SQLITE_VEC_SOURCE_DIR}/sqlite-vec.c
    PROPERTIES LANGUAGE C
)

if(UNIX)
    set_target_properties(knowledge_base_tools PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(WIN32)
    target_link_libraries(knowledge_base_tools PRIVATE "-static -static-libgcc -static-libstdc++ -lpthread")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(knowledge_base_tools PRIVATE
        Threads::Threads
        CURL::libcurl
        SQLite3::SQLite3
        m
    )
endif()

target_compile_definitions(knowledge_base_tools PRIVATE
    KNOWLEDGE_BASE_TOOLS_EXPORTS
    SQLITE_CORE
    SQLITE_VEC_STATIC
)

target_include_directories(knowledge_base_tools PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
    ${SQLITE_VEC_SOURCE_DIR}
    ${SQLITE_VEC_GENERATED_DIR}
)
