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

# Service-level integration tests: shared TestServer (one per suite) +
# SharedPgEnvironment (one per process) + per-test TruncateAll. Each
# service adds its own TEST_F(ServiceNameIT, ...) in Chunks 2-6 deriving
# from ServiceITBase. Uses a custom main() (NOT gtest_main) so it can
# install the SharedPgEnvironment as a global environment BEFORE the
# gtest framework runs.
add_executable(evgrpc_integration_tests
  service_integration_main.cc
  service_test_fixtures.cc
  test_data.cc              # data:: helpers (FreshUuid, MakeValid*...)
  vehicle_service_test.cc   # NEW — Chunk 2 Task 9
)
target_link_libraries(evgrpc_integration_tests PRIVATE
  gtest                # InitGoogleTest / RUN_ALL_TESTS / TEST_F
  # NO gtest_main — service_integration_main.cc provides its own main()
  evgrpc_test_fixtures
  evgrpc_proto
)
# Full-path include for `tests/integration/service_test_fixtures.h`
# (the fixture's own .cc uses `#include "tests/integration/..."`,
# which only resolves against the project root, not against the
# `tests/` include dir evgrpc_test_fixtures exposes).
target_include_directories(evgrpc_integration_tests PRIVATE
  ${CMAKE_SOURCE_DIR}
)
add_test(NAME evgrpc_integration_tests COMMAND evgrpc_integration_tests)

# Per-target coverage (see top-level CMakeLists.txt for why global fails).
if(EVGRPC_COVERAGE)
  foreach(target evgrpc_e2e_tests evgrpc_integration_tests)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE --coverage -O0 -g)
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endforeach()
endif()