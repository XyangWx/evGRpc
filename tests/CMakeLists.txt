add_executable(evgrpc_tests test_main.cc)
target_link_libraries(evgrpc_tests PRIVATE gtest gtest_main)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)