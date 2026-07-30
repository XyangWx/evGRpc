add_library(evgrpc_auth
  jwt_validator.cc
  jwks_cache.cc
)

# jwt-cpp is header-only and is exposed by the cmake/deps.cmake
# FetchContent (project name `jwt_cpp` -> `jwt-cpp::jwt-cpp` ALIAS, set in
# build/_deps/jwt_cpp-src/CMakeLists.txt:101). It transitively links
# OpenSSL::SSL + OpenSSL::Crypto, so we don't need to add OpenSSL here.
target_link_libraries(evgrpc_auth PUBLIC jwt-cpp::jwt-cpp)

# JWKS cache (Task 8) uses libcurl for the HTTP fetch and nlohmann_json
# for parsing the JWKS JSON. nlohmann_json::nlohmann_json is header-only;
# CURL::libcurl (the ALIAS for CURL::libcurl_static under our
# BUILD_SHARED_LIBS=OFF setting) transitively pulls in OpenSSL::SSL +
# OpenSSL::Crypto via its INTERFACE_LINK_LIBRARIES, so adding it to
# evgrpc_auth covers both JWK→PEM (RSA via OpenSSL) and the HTTP fetch.
target_link_libraries(evgrpc_auth PUBLIC
  CURL::libcurl
  nlohmann_json::nlohmann_json
)

# Tests reach into #include "auth/jwt_validator.h" and
# #include "fixtures/jwt_test_keys.h" — both live under src/ and tests/
# respectively. evgrpc_auth exposes src/ for "auth/..." consumers.
target_include_directories(evgrpc_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)