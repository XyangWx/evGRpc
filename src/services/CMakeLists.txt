add_library(evgrpc_services
  vehicle_service.cc
)
target_link_libraries(evgrpc_services PUBLIC
  evgrpc_db           # PgPool + ToGrpcStatus
  evgrpc_auth         # JwtValidator + Authenticate
  evgrpc_util         # NewUuid + RpcScope
  evgrpc_log          # spdlog named loggers
  evgrpc_proto_gen    # generated *.pb.{h,cc} + grpc.pb.{h,cc}
  grpc++
  protobuf::libprotobuf
  Threads::Threads
)
add_dependencies(evgrpc_services evgrpc_proto_gen)
target_include_directories(evgrpc_services PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated
)