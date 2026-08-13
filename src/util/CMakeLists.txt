add_library(evgrpc_util
  uuid.cc
  rpc_scope.cc
  req_id.cc
  args.cc
  last_day_of_month.cc
)
target_link_libraries(evgrpc_util PUBLIC
  evgrpc_log
  # grpc++ exposes grpcpp/support/{status,string_ref}.h headers; RpcScope
  # uses grpc::Status and grpc::string_ref directly.
  grpc++
  # libuuid is provided by the system (libuuid-dev on Debian/Ubuntu,
  # util-linux on RHEL/Fedora). CMake's FindUUID module exposes the
  # `uuid::uuid` imported target on modern CMake; here we use the
  # simpler library-name form which works on Ubuntu 22.04+.
  uuid
)
target_include_directories(evgrpc_util PUBLIC ${CMAKE_SOURCE_DIR}/src)