add_subdirectory(fixtures)

add_executable(evgrpc_tests
  test_main.cc
  unit/test_config.cc
  unit/test_pool.cc
  unit/test_error.cc
  unit/test_jwt_validator.cc
  unit/test_jwks_cache.cc
  unit/test_authenticate.cc
  unit/test_log.cc
)
target_link_libraries(evgrpc_tests PRIVATE
  gtest
  gtest_main
  evgrpc_auth
  evgrpc_config
  evgrpc_db
  evgrpc_log
  evgrpc_test_fixtures
  libpqxx::pqxx
)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)