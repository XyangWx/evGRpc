#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include "config/config.h"
#include "config/config_loader.h"

namespace {

std::string WriteTempJson(const std::string& content) {
  char path[] = "/tmp/evgrpc_test_runtime_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("mkstemp failed");
  close(fd);
  std::ofstream f(path);
  f << content;
  f.close();
  return std::string(path);
}

}  // namespace

TEST(ConfigRuntimeTest, RejectsNonHttpIssuer) {
  // Validate the schema's issuer-url check survives into LoadConfig —
  // even though LoadConfig would call OIDC discovery, the schema
  // rejects bad issuer URLs first.
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d" },
    "oauth": { "issuer_url": "ftp://auth.example.com", "audience": "x" },
    "grpc": {}, "log": {}
  })");
  EXPECT_THROW(evgrpc::LoadConfig(path), std::runtime_error);
  std::remove(path.c_str());
}

// Other LoadConfig behavior (success, OIDC failure) is integration-tested
// at Task 15 via scripts/smoke.sh and the e2e suite. Pure unit tests for
// LoadConfig would require either a real OIDC server or a heavy mock of
// httplib::Client; the schema validation it composes is already covered
// by ConfigLoaderTest.*.
