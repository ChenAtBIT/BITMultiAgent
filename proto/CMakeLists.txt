# 优先使用与 pkg-config 发现的 protobuf / gRPC 同前缀下的工具，
# 避免 shell PATH（例如 Conda）中的 protoc 与系统头文件版本不一致。
if(DEFINED PROTOBUF_PREFIX AND EXISTS "${PROTOBUF_PREFIX}/bin/protoc")
    set(PROTOC_EXECUTABLE "${PROTOBUF_PREFIX}/bin/protoc")
else()
    find_program(PROTOC_EXECUTABLE NAMES protoc REQUIRED)
endif()

if(DEFINED GRPC_PREFIX AND EXISTS "${GRPC_PREFIX}/bin/grpc_cpp_plugin")
    set(GRPC_CPP_PLUGIN_EXECUTABLE "${GRPC_PREFIX}/bin/grpc_cpp_plugin")
else()
    find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)
endif()

execute_process(
    COMMAND "${PROTOC_EXECUTABLE}" --version
    OUTPUT_VARIABLE PROTOC_VERSION_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX REPLACE "^libprotoc " "" PROTOC_VERSION "${PROTOC_VERSION_RAW}")

if(DEFINED PROTOBUF_VERSION AND NOT PROTOC_VERSION VERSION_EQUAL PROTOBUF_VERSION)
    message(WARNING
        "protoc version (${PROTOC_VERSION}) does not match protobuf library version "
        "(${PROTOBUF_VERSION}). Generated sources may be incompatible."
    )
endif()

message(STATUS "Using protoc: ${PROTOC_EXECUTABLE} (${PROTOC_VERSION_RAW})")
message(STATUS "Using grpc_cpp_plugin: ${GRPC_CPP_PLUGIN_EXECUTABLE}")

# 生成protobuf和gRPC代码
set(PROTO_FILES
    common.proto
    agent_service.proto
    ai_query.proto
)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    
    # 生成protobuf代码
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        COMMAND "${PROTOC_EXECUTABLE}"
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        COMMENT "Generating protobuf code for ${PROTO_FILE}"
    )
    
    # 生成gRPC代码
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h
        COMMAND "${PROTOC_EXECUTABLE}"
        ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
             --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}
        COMMENT "Generating gRPC code for ${PROTO_FILE}"
    )
    
    list(APPEND PROTO_SOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc
    )
    
    list(APPEND PROTO_HEADERS
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h
        ${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h
    )
endforeach()

# 创建proto库
add_library(proto_lib STATIC ${PROTO_SOURCES} ${PROTO_HEADERS})
target_link_libraries(proto_lib ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES})
target_include_directories(proto_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# 导出头文件
install(FILES ${PROTO_HEADERS} DESTINATION include/proto)
