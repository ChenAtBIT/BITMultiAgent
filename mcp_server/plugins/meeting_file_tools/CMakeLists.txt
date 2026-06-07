cmake_minimum_required(VERSION 3.10)

if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libgcc")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    link_libraries(pthread)
endif()

add_library(meeting_file_tools SHARED
    ${PROJECT_SOURCE_DIR}/plugins/meeting_file_tools/MeetingFileTools.cpp
)

if(UNIX)
    set_target_properties(meeting_file_tools PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(WIN32)
    target_link_libraries(meeting_file_tools PRIVATE "-static -static-libgcc -static-libstdc++ -lpthread")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(meeting_file_tools PRIVATE Threads::Threads)
endif()

target_compile_definitions(meeting_file_tools PRIVATE MEETING_FILE_TOOLS_EXPORTS)
target_include_directories(meeting_file_tools PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
