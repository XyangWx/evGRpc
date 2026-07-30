add_library(evgrpc_auth jwt_validator.cc)

# jwt-cpp is header-only and is exposed by the cmake/deps.cmake
# FetchContent (project name `jwt_cpp` -> `jwt-cpp::jwt-cpp` ALIAS, set in
# build/_deps/jwt_cpp-src/CMakeLists.txt:101). It transitively links
# OpenSSL::SSL + OpenSSL::Crypto, so we don't need to add OpenSSL here.
target_link_libraries(evgrpc_auth PUBLIC jwt-cpp::jwt-cpp)

# Tests reach into #include "auth/jwt_validator.h" and
# #include "fixtures/jwt_test_keys.h" — both live under src/ and tests/
# respectively. evgrpc_auth exposes src/ for "auth/..." consumers.
target_include_directories(evgrpc_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
