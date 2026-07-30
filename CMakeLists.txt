cmake_minimum_required(VERSION 3.22)
project(evgrpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/deps.cmake)
include(cmake/protoc.cmake)

# Subdirectories
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)