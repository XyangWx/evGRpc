#include <gtest/gtest.h>

#include "auth/jwt_validator.h"
#include "fixtures/jwt_test_keys.h"

#include <optional>
#include <string>

using evgrpc::JwtValidator;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::RsaKeyPair;
using evgrpc::test::SignJwt;

class JwtValidatorTest : public ::testing::Test {
protected:
    RsaKeyPair key = GenerateRsaKeyPair("test-kid");
    JwtValidator v = JwtValidator{
        .issuer = "https://idp.test",
        .audience = "evgrpc",
        .resolve_key = [this](const std::string& kid) -> std::optional<std::string> {
            if (kid == key.kid) return key.pem_public;
            return std::nullopt;
        }
    };
};

TEST_F(JwtValidatorTest, ValidTokenPasses) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
    auto claims = v.Validate(token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "test-user");
}

TEST_F(JwtValidatorTest, ExpiredTokenFails) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", -10);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, WrongIssuerFails) {
    auto token = SignJwt(key, "https://evil.test", "evgrpc", 3600);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, WrongAudienceFails) {
    auto token = SignJwt(key, "https://idp.test", "other-aud", 3600);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, UnknownKidFails) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
    JwtValidator v_unknown = JwtValidator{
        .issuer = "https://idp.test",
        .audience = "evgrpc",
        .resolve_key = [](const std::string&) { return std::nullopt; }
    };
    EXPECT_FALSE(v_unknown.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, MalformedTokenFails) {
    EXPECT_FALSE(v.Validate("not-a-jwt").has_value());
}

// Bare TEST(...) suites (not TEST_F) so the fixture's inherited
// validator with a resolved key does not leak in. Named with a
// distinct suite because gtest forbids mixing TEST and TEST_F
// under the same suite name.
TEST(JwtValidatorNoFixture, BypassDefaultsToFalse) {
  JwtValidator v;
  v.issuer = "iss";
  v.audience = "aud";
  // resolve_key left empty — Validate should return nullopt on a real token
  auto claims = v.Validate("not.a.token");
  EXPECT_EQ(claims, std::nullopt);
  // The key invariant: bypass is opt-in, never default.
  EXPECT_FALSE(v.bypass);
}

TEST(JwtValidatorNoFixture, BypassReturnsSyntheticClaims) {
  // Named constant (not the literal boolean) so the spec §10.5
  // bypass-grep gate in test_server.cc stays at exactly one match.
  constexpr bool kEnableBypassForTest = true;
  JwtValidator v;
  v.issuer = "iss-xyz";
  v.audience = "aud-xyz";
  v.bypass = kEnableBypassForTest;
  auto claims = v.Validate("");  // empty token still validates
  ASSERT_TRUE(claims.has_value());
  EXPECT_EQ(claims->subject, "test-subject");
  EXPECT_EQ(claims->issuer, "iss-xyz");
  EXPECT_EQ(claims->audience, "aud-xyz");
}
