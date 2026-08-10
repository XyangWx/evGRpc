#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include "config/config_loader.h"

namespace {

// Write `content` to a temp file and return the path. Caller is
// responsible for cleanup (or letting /tmp reap on reboot).
std::string WriteTempJson(const std::string& content) {
  char path[] = "/tmp/evgrpc_test_config_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("mkstemp failed");
  close(fd);
  std::ofstream f(path);
  f << content;
  f.close();
  return std::string(path);
}

constexpr char kValid[] = R"({
  "database": { "url": "postgresql://u:p@h:5432/d" },
  "oauth": { "issuer_url": "https://auth.example.com", "audience": "evgrpc-api" },
  "grpc": { "port": 50051 },
  "log": { "level": "info", "file": "" }
})";

}  // namespace

TEST(ConfigLoaderTest, LoadsValidConfig) {
  auto path = WriteTempJson(kValid);
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url, "postgresql://u:p@h:5432/d");
  EXPECT_EQ(cfg.oauth.issuer_url, "https://auth.example.com");
  EXPECT_EQ(cfg.oauth.audience, "evgrpc-api");
  EXPECT_EQ(cfg.oauth.jwks_cache_ttl_seconds, 3600);  // default
  EXPECT_EQ(cfg.grpc.port, 50051);
  EXPECT_EQ(cfg.log.level, "info");
  EXPECT_EQ(cfg.log.file, "");
  EXPECT_EQ(cfg.log.max_size_mb, 100);  // default
  EXPECT_EQ(cfg.log.max_files, 7);      // default
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, AppliesAllDefaults) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": {},
    "log": {}
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.oauth.jwks_cache_ttl_seconds, 3600);
  EXPECT_EQ(cfg.grpc.port, 50051);
  EXPECT_EQ(cfg.log.level, "info");
  EXPECT_EQ(cfg.log.file, "");
  EXPECT_EQ(cfg.log.max_size_mb, 100);
  EXPECT_EQ(cfg.log.max_files, 7);
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsMissingDatabaseUrl) {
  auto path = WriteTempJson(R"({
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": {}, "log": {}
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("database.url"), std::string::npos) << msg;
  }
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsBadDatabaseUrlPrefix) {
  auto path = WriteTempJson(R"({
    "database": { "url": "mysql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": {}, "log": {}
  })");
  EXPECT_THROW(evgrpc::LoadSchema(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsInvalidIssuerUrl) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "not-a-url", "audience": "x" },
    "grpc": {}, "log": {}
  })");
  EXPECT_THROW(evgrpc::LoadSchema(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsInvalidLogLevel) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": {}, "log": { "level": "verbose" }
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("log.level"), std::string::npos) << msg;
  }
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsOutOfRangePort) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 70000 }, "log": {}
  })");
  EXPECT_THROW(evgrpc::LoadSchema(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsUnknownKey) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x", "tenant_id": "abc" },
    "grpc": {}, "log": {}
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("tenant_id"), std::string::npos) << msg;
  }
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsMalformedJson) {
  auto path = WriteTempJson("{ this is not json");
  EXPECT_THROW(evgrpc::LoadSchema(path), std::runtime_error);
  std::remove(path.c_str());
}

TEST(ConfigLoaderTest, RejectsMissingFile) {
  EXPECT_THROW(evgrpc::LoadSchema("/nonexistent/path/that/does/not/exist.json"),
               std::runtime_error);
}

TEST(ConfigLoaderTest, CollectsAllErrors) {
  // Multiple problems in one file: bad database url, invalid log level,
  // out-of-range port. Expect all three to be reported in one message.
  auto path = WriteTempJson(R"({
    "database": { "url": "mysql://u@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": -1 },
    "log": { "level": "verbose" }
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("database.url"), std::string::npos) << msg;
    EXPECT_NE(msg.find("log.level"), std::string::npos) << msg;
    EXPECT_NE(msg.find("grpc.port"), std::string::npos) << msg;
  }
  std::remove(path.c_str());
}

// ===========================================================================
// EVGRPC_DATABASE_URL env-var fallback tests
//
// Source priority: config.json > env var. Json wins when the field is
// present and non-empty; env var only takes over when json omits the field,
// has an empty string, or has the wrong type. Each test sets / unsets the
// env var via setenv/unsetenv so they're isolated from each other and from
// the host shell.
// ===========================================================================

class EnvVarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Snapshot whether the var was set on entry (so we don't leak state
    // into other tests if the host shell had it set).
    const char* existing = std::getenv("EVGRPC_DATABASE_URL");
    had_env_on_entry_ = (existing != nullptr && *existing != '\0');
    if (had_env_on_entry_) prior_value_ = existing;
    unsetenv("EVGRPC_DATABASE_URL");
  }
  void TearDown() override {
    if (had_env_on_entry_) {
      setenv("EVGRPC_DATABASE_URL", prior_value_.c_str(), 1);
    } else {
      unsetenv("EVGRPC_DATABASE_URL");
    }
  }
  bool had_env_on_entry_;
  std::string prior_value_;
};

TEST_F(EnvVarTest, JsonUrlWinsOverEnvVar) {
  // json has a valid url; env also has one. json wins.
  setenv("EVGRPC_DATABASE_URL",
         "postgresql://envuser:envpass@envhost:5432/envdb", 1);
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://jsonuser:jsonpass@jsonhost:5432/jsondb" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url,
            "postgresql://jsonuser:jsonpass@jsonhost:5432/jsondb");
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, EnvVarUsedWhenJsonMissingUrl) {
  // json omits url; env has a valid one.
  setenv("EVGRPC_DATABASE_URL",
         "postgresql://envuser:envpass@envhost:5432/envdb", 1);
  auto path = WriteTempJson(R"({
    "database": {},
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url,
            "postgresql://envuser:envpass@envhost:5432/envdb");
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, EnvVarUsedWhenJsonEmptyUrl) {
  // json has url: ""; env has a valid one.
  setenv("EVGRPC_DATABASE_URL",
         "postgresql://envuser@h/d", 1);
  auto path = WriteTempJson(R"({
    "database": { "url": "" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url, "postgresql://envuser@h/d");
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, EnvVarUsedWhenJsonSectionMissing) {
  // json omits the entire database section; env has a valid one.
  setenv("EVGRPC_DATABASE_URL", "postgresql://env@h/d", 1);
  auto path = WriteTempJson(R"({
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url, "postgresql://env@h/d");
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, ErrorsWhenNeitherJsonNorEnv) {
  // json omits url AND env is unset. Expect the helpful error message
  // mentioning both options.
  auto path = WriteTempJson(R"({
    "database": {},
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("database.url"), std::string::npos) << msg;
    EXPECT_NE(msg.find("EVGRPC_DATABASE_URL"), std::string::npos)
        << "error should mention the env var name so user knows the "
           "alternative config path. Got: " << msg;
  }
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, EnvVarWithBadPrefixReportsError) {
  // Env var exists but doesn't start with postgresql:// — error must
  // mention EVGRPC_DATABASE_URL so the user knows which source failed.
  setenv("EVGRPC_DATABASE_URL", "mysql://env@h/d", 1);
  auto path = WriteTempJson(R"({
    "database": {},
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  try {
    evgrpc::LoadSchema(path);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("postgresql://"), std::string::npos) << msg;
    EXPECT_NE(msg.find("EVGRPC_DATABASE_URL"), std::string::npos) << msg;
  }
  std::remove(path.c_str());
}

TEST_F(EnvVarTest, EmptyEnvVarTreatedAsUnset) {
  // Env var set to "" is treated as unset (matches getenv null-guard).
  setenv("EVGRPC_DATABASE_URL", "", 1);
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://json@h/d" },
    "oauth": { "issuer_url": "https://a", "audience": "x" },
    "grpc": { "port": 50051 },
    "log": { "level": "info", "file": "" }
  })");
  auto cfg = evgrpc::LoadSchema(path);
  EXPECT_EQ(cfg.database.url, "postgresql://json@h/d");
  std::remove(path.c_str());
}
