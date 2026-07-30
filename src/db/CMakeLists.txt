add_library(evgrpc_db pool.cc error.cc)
# libpqxx's CMakeLists adds `libpqxx::pqxx` as an ALIAS for target `pqxx`,
# then exports it under the `libpqxx::` namespace. The plan/brief wrote
# `pqxx::pqxx` which doesn't match what libpqxx 7.9.2 actually exposes.
target_link_libraries(evgrpc_db PUBLIC libpqxx::pqxx)
# gRPC's CMakeLists does NOT define a `gRPC::grpc++` ALIAS — that namespace
# is only applied at install time via `install(EXPORT ... NAMESPACE gRPC::)`,
# which FetchContent does not run. With FetchContent, the bare target is
# `grpc++` (verified by grepping build/_deps/grpc-src/CMakeLists.txt:4002 —
# only `add_library(grpc++ ...)`, no ALIAS). Same pattern as the existing
# protoc.cmake comment about `grpc_cpp_plugin`.
target_link_libraries(evgrpc_db PUBLIC grpc++)
target_include_directories(evgrpc_db PUBLIC ${CMAKE_SOURCE_DIR}/src)
