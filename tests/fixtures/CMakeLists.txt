# Static library so evgrpc_tests (and downstream test executables) can
# reuse the fixture code without recompiling per TU.
add_library(evgrpc_test_fixtures STATIC
  jwt_test_keys.cc
  pg_container.cc
  shared_pg.cc
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
  gtest               # SharedPgEnvironment derives from ::testing::Environment
)
# Combined schema loader: 001_initial.sql + 002_charging_timestamptz_migration.sql
# + 003_consumption_timestamptz_migration.sql. Concatenated at configure
# time so tests always apply all three. Fresh installs get TIMESTAMPTZ
# via 001; existing DBs get migrated by 002 + 003.
file(READ "${CMAKE_SOURCE_DIR}/sql/001_initial.sql" _sql_001)
file(READ "${CMAKE_SOURCE_DIR}/sql/002_charging_timestamptz_migration.sql" _sql_002)
file(READ "${CMAKE_SOURCE_DIR}/sql/003_consumption_timestamptz_migration.sql" _sql_003)
file(WRITE "${CMAKE_BINARY_DIR}/generated/test_schema.sql"
     "${_sql_001}\n${_sql_002}\n${_sql_003}\n")
# PUBLIC (not PRIVATE) so consumers (evgrpc_tests, evgrpc_integration_tests)
# see these definitions when compiling their own sources. The existing
# line at tests/fixtures/CMakeLists.txt:39 also uses PUBLIC — keep
# the visibility consistent so the migration path is visible to the
# new unit-test file in evgrpc_tests.
target_compile_definitions(evgrpc_test_fixtures PUBLIC
  EVGRPC_TEST_SQL_PATH="${CMAKE_BINARY_DIR}/generated/test_schema.sql"
  EVGRPC_MIGRATION_002_PATH="${CMAKE_SOURCE_DIR}/sql/002_charging_timestamptz_migration.sql"
  EVGRPC_MIGRATION_003_PATH="${CMAKE_SOURCE_DIR}/sql/003_consumption_timestamptz_migration.sql"
)
# Per-target coverage (see top-level CMakeLists.txt for why global fails).
if(EVGRPC_COVERAGE)
  target_compile_options(evgrpc_test_fixtures PRIVATE --coverage -O0 -g)
  target_link_options(evgrpc_test_fixtures PRIVATE --coverage)
endif()
# PG container + httplib need C++20 (matches our top-level).
target_compile_features(evgrpc_test_fixtures PUBLIC cxx_std_20)