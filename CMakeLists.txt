cmake_minimum_required(VERSION 3.22)
project(evgrpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Coverage option is declared at top-level for visibility, but the
# actual flag injection happens per-target in src/CMakeLists.txt and
# tests/CMakeLists.txt — adding it via add_compile_options globally
# propagates to FetchContent deps (notably grpc_cpp_plugin) and breaks
# their link with gcov instrumentation, see spec review at commit
# `df6b58f` for the failure mode.
option(EVGRPC_COVERAGE "Enable coverage instrumentation (gcov)" OFF)

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