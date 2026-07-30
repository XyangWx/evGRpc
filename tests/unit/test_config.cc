#include <gtest/gtest.h>
#include "config/config.h"

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override { clear_env(); }
    void clear_env() {
        unsetenv("DATABASE_URL");
        unsetenv("OAUTH_ISSUER_URL");
        unsetenv("OAUTH_AUDIENCE");
        unsetenv("OAUTH_JWKS_URL");
        unsetenv("OAUTH_JWKS_CACHE_TTL");
        unsetenv("GRPC_PORT");
    }
};

TEST_F(ConfigTest, LoadsAllRequiredVars) {
    setenv("DATABASE_URL", "postgres://x", 1);
    setenv("OAUTH_ISSUER_URL", "https://idp", 1);
    setenv("OAUTH_AUDIENCE", "evgrpc", 1);
    setenv("OAUTH_JWKS_URL", "https://idp/jwks", 1);
    auto c = evgrpc::Config::Load();
    EXPECT_EQ(c.database_url, "postgres://x");
    EXPECT_EQ(c.oauth_audience, "evgrpc");
    EXPECT_EQ(c.grpc_port, 50051);  // default
}

TEST_F(ConfigTest, MissingRequiredThrows) {
    setenv("DATABASE_URL", "postgres://x", 1);
    // other required vars missing
    EXPECT_THROW(evgrpc::Config::Load(), std::runtime_error);
}
