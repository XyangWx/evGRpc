add_library(evgrpc_db pool.cc)
# libpqxx's CMakeLists adds `libpqxx::pqxx` as an ALIAS for target `pqxx`,
# then exports it under the `libpqxx::` namespace. The plan/brief wrote
# `pqxx::pqxx` which doesn't match what libpqxx 7.9.2 actually exposes.
target_link_libraries(evgrpc_db PUBLIC libpqxx::pqxx)
target_include_directories(evgrpc_db PUBLIC ${CMAKE_SOURCE_DIR}/src)
