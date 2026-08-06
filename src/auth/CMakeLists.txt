add_library(evgrpc_auth
  jwt_validator.cc
  jwks_cache.cc
  authenticate.cc   # Task 9 — per-RPC Bearer JWT helper
  oidc_discovery.cc # Task 3 — GET /.well-known/openid-configuration
)

# OIDC discovery (Task 3) GETs {issuer}/.well-known/openid-configuration
# at startup and parses the JSON body for the jwks_uri field. Uses
# cpp-httplib's httplib::Client (http only on this code path) and
# nlohmann_json for body parsing — both already available through
# evgrpc_auth's existing transitive deps (nlohmann_json via jwks_cache,
# cpp-httplib via evgrpc_test_fixtures which is a sibling target — but
# evgrpc_auth itself does not depend on httplib yet, so link it now).
target_link_libraries(evgrpc_auth PUBLIC
  httplib::httplib
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
  # Task 9's Authenticate helper uses grpc::Status and grpc::string_ref
  # (from <grpcpp/support/status.h> and <grpcpp/support/string_ref.h>).
  # We only need the public headers + grpc++ static lib — no codegen, no
  # protobuf runtime symbol emission.
  grpc++
)

# Tests reach into #include "auth/jwt_validator.h" and
# #include "fixtures/jwt_test_keys.h" — both live under src/ and tests/
# respectively. evgrpc_auth exposes src/ for "auth/..." consumers.
target_include_directories(evgrpc_auth PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)