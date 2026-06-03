cmake_minimum_required(VERSION 3.10)

add_library(tutorial_echo SHARED
    ${PROJECT_SOURCE_DIR}/plugins/tutorial_echo/TutorialEcho.cpp
)

if(UNIX)
    set_target_properties(tutorial_echo PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(WIN32)
    target_link_libraries(tutorial_echo PRIVATE "-static -static-libgcc -static-libstdc++ -lpthread")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tutorial_echo PRIVATE Threads::Threads)
endif()

target_include_directories(tutorial_echo PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
