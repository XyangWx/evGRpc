add_subdirectory(fixtures)
add_subdirectory(integration)

add_executable(evgrpc_tests
  test_main.cc
  unit/test_config_loader.cc
  unit/test_config_runtime.cc
  unit/test_db_exec.cc
  unit/test_pool.cc
  unit/test_error.cc
  unit/test_jwt_validator.cc
  unit/test_jwks_cache.cc
  unit/test_authenticate.cc
  unit/test_oidc_discovery.cc
  unit/test_log.cc
  unit/test_args.cc
  unit/test_pool_events.cc
  unit/uuid_test.cc
  unit/rpc_scope_test.cc
  unit/test_pg_container.cc
  unit/test_pg_container_config.cc
  unit/test_schema_migration.cc
  unit/test_last_day_of_month.cc
)
target_link_libraries(evgrpc_tests PRIVATE
  gtest
  gtest_main
  evgrpc_auth
  evgrpc_config
  evgrpc_db
  evgrpc_log
  evgrpc_util
  evgrpc_test_fixtures
  libpqxx::pqxx
)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)

# Per-target coverage (see top-level CMakeLists.txt for why global fails).
if(EVGRPC_COVERAGE)
  target_compile_options(evgrpc_tests PRIVATE --coverage -O0 -g)
  target_link_options(evgrpc_tests PRIVATE --coverage)
endif()