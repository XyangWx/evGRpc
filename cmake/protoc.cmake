include(FetchContent)
find_package(Protobuf REQUIRED)
# NOTE: find_package(gRPC) does not work with grpc fetched via FetchContent
# (gRPCConfig.cmake is generated under _deps/grpc-build but not installed into a
# search path, and the brief's `gRPC::grpc_cpp_plugin` target name is wrong).
# grpc exposes its plugin as the bare target `grpc_cpp_plugin` once
# FetchContent_MakeAvailable has added grpc via add_subdirectory, so we use it
# directly via $<TARGET_FILE:grpc_cpp_plugin>.

set(EVGRPC_PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(EVGRPC_PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)

file(MAKE_DIRECTORY ${EVGRPC_PROTO_GEN_DIR})

set(EVGRPC_PROTO_FILES
  ${EVGRPC_PROTO_DIR}/evgrpc/common.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/vehicle.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/weather.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/source_category.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/consumption.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/charging.proto
  ${EVGRPC_PROTO_DIR}/evgrpc/display.proto
)

add_custom_target(evgrpc_proto_gen
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    --proto_path=${EVGRPC_PROTO_DIR}
    --cpp_out=${EVGRPC_PROTO_GEN_DIR}
    --grpc_out=${EVGRPC_PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    ${EVGRPC_PROTO_FILES}
  DEPENDS ${EVGRPC_PROTO_FILES}
  COMMENT "Generating protobuf + gRPC stubs"
  VERBATIM
)