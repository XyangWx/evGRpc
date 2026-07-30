add_executable(evgrpc_tests test_main.cc unit/test_config.cc)
target_link_libraries(evgrpc_tests PRIVATE gtest gtest_main evgrpc_config)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)