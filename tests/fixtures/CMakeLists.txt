# Static library so evgrpc_tests (and downstream test executables) can
# reuse the fixture code without recompiling per TU.
add_library(evgrpc_test_fixtures STATIC
  jwt_test_keys.cc
  pg_container.cc
  test_server.cc
)
# Expose the generated/ header dir to consumers.
target_include_directories(evgrpc_test_fixtures PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_include_directories(evgrpc_test_fixtures PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# OpenSSL is needed for EVP_PKEY + RSA keygen + PEM↔JWK conversion.
# jwt-cpp's INTERFACE library transitively pulls in OpenSSL::SSL +
# OpenSSL::Crypto, so linking jwt-cpp also brings OpenSSL.
target_link_libraries(evgrpc_test_fixtures PUBLIC
  jwt-cpp::jwt-cpp
  testcontainers::testcontainers
  testcontainers::modules
)
# httplib + gRPC + libpqxx + generated stubs.
target_link_libraries(evgrpc_test_fixtures PUBLIC
  evgrpc_services     # all 6 service Impls
  evgrpc_proto        # generated *.pb.h / *.grpc.pb.h
  httplib::httplib    # bundled via cpp_httplib FetchContent
  grpc++
  protobuf::libprotobuf
  libpqxx::pqxx
  Threads::Threads
)
# PG container + httplib need C++20 (matches our top-level).
target_compile_features(evgrpc_test_fixtures PUBLIC cxx_std_20)