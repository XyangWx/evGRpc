cmake_minimum_required(VERSION 3.22)
project(evgrpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EVGRPC_COVERAGE "Enable coverage instrumentation (gcov)" OFF)
if(EVGRPC_COVERAGE)
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()

include(cmake/deps.cmake)
include(cmake/protoc.cmake)

# Subdirectories
add_subdirectory(src)

# Tests are gated by EVGRPC_BUILD_TESTS so that production Docker
# images can skip them without copying the tests/ directory into the
# image at all.
option(EVGRPC_BUILD_TESTS "Build unit + integration tests" ON)
if(EVGRPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()