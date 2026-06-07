cmake_minimum_required(VERSION 3.10)

if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libgcc")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    link_libraries(pthread)
endif()

add_library(server_observer SHARED
    ${PROJECT_SOURCE_DIR}/plugins/server_observer/ServerObserver.cpp
)

if(UNIX)
    set_target_properties(server_observer PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(WIN32)
    target_link_libraries(server_observer PRIVATE "-static -static-libgcc -static-libstdc++ -lpthread")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(server_observer PRIVATE Threads::Threads)
endif()

target_compile_definitions(server_observer PRIVATE SERVER_OBSERVER_EXPORTS)
target_include_directories(server_observer PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
