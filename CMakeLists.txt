cmake_minimum_required(VERSION 3.16)
project(grpc_load_balancer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# gRPC and protobuf are located via pkg-config rather than find_package(gRPC).
# Homebrew's gRPC ships working .pc files, and pkg-config resolves the long
# transitive dependency chain (absl, re2, c-ares, openssl...) for us. The
# IMPORTED_TARGET form gives us PkgConfig::GRPCPP carrying include paths, link
# paths and libraries together, so consumers just link the target.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(Threads REQUIRED)

find_program(PROTOC_BIN protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_BIN grpc_cpp_plugin
             HINTS /opt/homebrew/opt/grpc/bin REQUIRED)

message(STATUS "protoc            : ${PROTOC_BIN}")
message(STATUS "grpc_cpp_plugin   : ${GRPC_CPP_PLUGIN_BIN}")
message(STATUS "grpc++ version    : ${GRPCPP_VERSION}")

# ---------------------------------------------------------------------------
# Protobuf / gRPC code generation
#
# protoc turns each .proto into four files: <name>.pb.{h,cc} (the message
# types) and <name>.grpc.pb.{h,cc} (the client stubs and server base classes).
# They are generated into the build tree, never committed -- the .proto is the
# source of truth.
# ---------------------------------------------------------------------------
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_NAMES echo health)
set(PROTO_GEN_SRCS "")

foreach(name ${PROTO_NAMES})
  set(_gen
    ${PROTO_GEN_DIR}/${name}.pb.cc
    ${PROTO_GEN_DIR}/${name}.pb.h
    ${PROTO_GEN_DIR}/${name}.grpc.pb.cc
    ${PROTO_GEN_DIR}/${name}.grpc.pb.h)

  add_custom_command(
    OUTPUT ${_gen}
    COMMAND ${PROTOC_BIN}
            --proto_path=${PROTO_SRC_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_BIN}
            ${PROTO_SRC_DIR}/${name}.proto
    DEPENDS ${PROTO_SRC_DIR}/${name}.proto
    COMMENT "protoc: ${name}.proto"
    VERBATIM)

  list(APPEND PROTO_GEN_SRCS
       ${PROTO_GEN_DIR}/${name}.pb.cc
       ${PROTO_GEN_DIR}/${name}.grpc.pb.cc)
endforeach()

add_library(proto_gen STATIC ${PROTO_GEN_SRCS})
target_include_directories(proto_gen PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(proto_gen PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)

# Generated protobuf code trips -Wunused-parameter and friends; it is not ours
# to fix, so keep our warnings off it.
target_compile_options(proto_gen PRIVATE -w)

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------
add_library(lb_core STATIC
  src/lb/backend_pool.cc
  src/lb/metrics_server.cc)
target_include_directories(lb_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/lb)
target_link_libraries(lb_core PUBLIC proto_gen Threads::Threads)
target_compile_options(lb_core PRIVATE -Wall -Wextra)

add_executable(lb_server src/lb/lb_server.cc)
target_link_libraries(lb_server PRIVATE lb_core)
target_compile_options(lb_server PRIVATE -Wall -Wextra)

add_executable(backend_server src/backend/backend_server.cc)
target_link_libraries(backend_server PRIVATE proto_gen Threads::Threads)
target_compile_options(backend_server PRIVATE -Wall -Wextra)

add_executable(load_client src/client/load_client.cc)
target_include_directories(load_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(load_client PRIVATE proto_gen Threads::Threads)
target_compile_options(load_client PRIVATE -Wall -Wextra)
