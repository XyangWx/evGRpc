#include <gtest/gtest.h>
#include "auth/authenticate.h"
#include "auth/jwt_validator.h"
#include "fixtures/jwt_test_keys.h"
#include <map>
#include <string>

using evgrpc::Authenticate;
using evgrpc::JwtValidator;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::RsaKeyPair;
using evgrpc::test::SignJwt;

namespace {

// Build a client_metadata multimap carrying the given Authorization header
// value. (Used as input to Authenticate; accepts a header value test owns
// so the resulting string_ref points into stable storage for the call.)
std::multimap<grpc::string_ref, grpc::string_ref> WithAuth(
    const std::string& header_value) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  md.emplace(grpc::string_ref("authorization"),
             grpc::string_ref(header_value.data(), header_value.size()));
  return md;
}

// std::string::find-based "contains" — gmock isn't linked into evgrpc_tests.
testing::AssertionResult Contains(const grpc::Status& status,
                                  const std::string& needle) {
  if (status.error_message().find(needle) != std::string::npos) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure()
         << "expected error_message to contain \"" << needle
         << "\", got \"" << status.error_message() << "\"";
}

}  // namespace

class AuthenticateTest : public ::testing::Test {
 protected:
  RsaKeyPair key = GenerateRsaKeyPair("test-kid");
  JwtValidator v = JwtValidator{
      .issuer = "https://idp.test",
      .audience = "evgrpc",
      .resolve_key = [this](const std::string& kid) -> std::optional<std::string> {
        if (kid == key.kid) return key.pem_public;
        return std::nullopt;
      }};
};

TEST_F(AuthenticateTest, NoAuthHeaderReturnsUnauthenticated) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  auto status = Authenticate(md, v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(status, "missing")) << status.error_message();
}

TEST_F(AuthenticateTest, NonBearerAuthReturnsUnauthenticated) {
  auto status = Authenticate(WithAuth("Basic dXNlcjpwYXNz"), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(status, "Bearer")) << status.error_message();
}

TEST_F(AuthenticateTest, ValidTokenReturnsOk) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST_F(AuthenticateTest, ExpiredTokenReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", -10);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

TEST_F(AuthenticateTest, WrongIssuerReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://evil.test", "evgrpc", 3600);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

TEST_F(AuthenticateTest, EmptyBearerReturnsUnauthenticated) {
  auto status = Authenticate(WithAuth("Bearer "), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}
