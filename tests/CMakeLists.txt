add_executable(evgrpc_tests test_main.cc unit/test_config.cc unit/test_pool.cc unit/test_error.cc)
target_link_libraries(evgrpc_tests PRIVATE gtest gtest_main evgrpc_config evgrpc_db libpqxx::pqxx)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)