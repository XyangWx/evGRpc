#include <gtest/gtest.h>
#include "auth/authenticate.h"
#include "auth/jwt_validator.h"
#include "fixtures/jwt_test_keys.h"
#include <map>
#include <string>

using evgrpc::Authenticate;
using evgrpc::Claims;
using evgrpc::JwtValidator;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::RsaKeyPair;
using evgrpc::test::SignJwt;

namespace {

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
  std::string reason;
  auto status = Authenticate(md, v, /*out_claims=*/nullptr, &reason);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(status, "missing")) << status.error_message();
  EXPECT_EQ(reason, "missing_header");
}

TEST_F(AuthenticateTest, NonBearerAuthReturnsUnauthenticated) {
  std::string reason;
  auto status = Authenticate(WithAuth("Basic dXNlcjpwYXNz"), v,
                             /*out_claims=*/nullptr, &reason);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_TRUE(Contains(status, "Bearer")) << status.error_message();
  EXPECT_EQ(reason, "non_bearer");
}

TEST_F(AuthenticateTest, ValidTokenReturnsOk) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
  Claims claims;
  std::string reason;
  auto status = Authenticate(WithAuth("Bearer " + token), v, &claims, &reason);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(reason, "ok");
  EXPECT_EQ(claims.subject, "test-user");
  EXPECT_EQ(claims.issuer, "https://idp.test");
  EXPECT_EQ(claims.audience, "evgrpc");
}

TEST_F(AuthenticateTest, ExpiredTokenReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", -10);
  std::string reason;
  auto status = Authenticate(WithAuth("Bearer " + token), v,
                             /*out_claims=*/nullptr, &reason);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(reason, "bad_signature");
}

TEST_F(AuthenticateTest, WrongIssuerReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://evil.test", "evgrpc", 3600);
  std::string reason;
  auto status = Authenticate(WithAuth("Bearer " + token), v,
                             /*out_claims=*/nullptr, &reason);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(reason, "bad_signature");
}

TEST_F(AuthenticateTest, EmptyBearerReturnsUnauthenticated) {
  std::string reason;
  auto status = Authenticate(WithAuth("Bearer "), v,
                             /*out_claims=*/nullptr, &reason);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(reason, "non_bearer");
}

// Task 10.5: subject must propagate through Authenticate to RpcScope
// via the out_claims parameter (rather than the service method having to
// re-parse the token). On success the helper fills claims.subject with
// the JWT `sub`; on failure claims is unchanged.
TEST_F(AuthenticateTest, OutClaimsFilledOnSuccess) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
  Claims claims;  // default-constructed — subject etc. empty
  ASSERT_TRUE(claims.subject.empty());

  auto status = Authenticate(WithAuth("Bearer " + token), v, &claims);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(claims.subject.empty());
  EXPECT_EQ(claims.subject, "test-user");
}

TEST_F(AuthenticateTest, OutClaimsUnchangedOnFailure) {
  Claims claims;
  claims.subject = "should-stay-untouched";

  auto status = Authenticate(WithAuth("Bearer not-a-jwt"), v, &claims);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(claims.subject, "should-stay-untouched");
}

TEST_F(AuthenticateTest, OutClaimsNullptrIsAccepted) {
  // nullptr out_claims must not crash; just returns OK.
  auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
  auto status = Authenticate(WithAuth("Bearer " + token), v,
                             /*out_claims=*/nullptr);
  EXPECT_TRUE(status.ok()) << status.error_message();
}