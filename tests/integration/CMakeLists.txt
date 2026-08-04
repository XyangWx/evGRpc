# Integration tests: full stack — PG (via testcontainers) + gRPC server
# (via TestServer) + generated client stubs. Heavier than unit tests
# (~10s per test: PG image pull on first run, in-process gRPC bringup),
# so they live in their own binary to keep the fast unit-test loop fast.
add_executable(evgrpc_e2e_tests
  smoke_e2e_test.cc
)
target_link_libraries(evgrpc_e2e_tests PRIVATE
  gtest
  gtest_main
  evgrpc_test_fixtures
  evgrpc_proto         # generated stubs (VehicleService::NewStub etc.)
)
add_test(NAME evgrpc_e2e_tests COMMAND evgrpc_e2e_tests)