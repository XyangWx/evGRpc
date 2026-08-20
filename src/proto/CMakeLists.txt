# Static library wrapping the generated protobuf + gRPC stubs.
# Each `*.pb.cc` + `*.grpc.pb.cc` is the implementation of one proto
# file's messages / service base class. They're produced by
# `evgrpc_proto_gen` (cmake/protoc.cmake) but that target is a UTILITY
# (just runs the protoc command); it does not compile anything. Linking
# them into a real library is what gives downstream consumers (services,
# server binary, e2e tests) the symbols.
add_library(evgrpc_proto
  ${CMAKE_BINARY_DIR}/generated/evgrpc/common.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/common.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/vehicle.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/vehicle.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/weather.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/weather.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/source_category.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/source_category.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/consumption.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/consumption.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/charging.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/charging.grpc.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/display.pb.cc
  ${CMAKE_BINARY_DIR}/generated/evgrpc/display.grpc.pb.cc
  # Vendored google.type.Date. Compiled like any other proto-generated
  # file but lives outside the evgrpc/ tree.
  ${CMAKE_BINARY_DIR}/generated/google/type/date.pb.cc
)
target_include_directories(evgrpc_proto PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(evgrpc_proto PUBLIC
  protobuf::libprotobuf
  grpc++
)
add_dependencies(evgrpc_proto evgrpc_proto_gen)