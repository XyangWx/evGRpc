# Static library so evgrpc_tests can use the helper functions
# (GenerateRsaKeyPair, SignJwt) without recompiling per TU.
add_library(evgrpc_test_fixtures STATIC jwt_test_keys.cc)

# OpenSSL is needed for EVP_PKEY + RSA keygen.
# jwt-cpp's INTERFACE library transitively pulls in OpenSSL::SSL +
# OpenSSL::Crypto, so linking jwt-cpp also brings OpenSSL.
target_link_libraries(evgrpc_test_fixtures PUBLIC jwt-cpp::jwt-cpp)

# Test sources include via #include "fixtures/jwt_test_keys.h" — the
# header is at tests/fixtures/jwt_test_keys.h, so we expose the *parent*
# (tests/) on the include path. That way a #include "fixtures/..." from
# tests/unit/test_jwt_validator.cc resolves to tests/fixtures/...
target_include_directories(evgrpc_test_fixtures PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
