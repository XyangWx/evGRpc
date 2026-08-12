// Unit tests for ReadDatabaseUrlFromConfig — the fallback path that
// lets PgContainer read database.url from ./config.json when neither
// DATABASE_URL nor EVGRPC_TEST_DB_PASSWORD is set.
//
// User pointed out (rightly) that the previous unit test suite didn't
// exercise this fallback: tests/integration/*.cc rely on DATABASE_URL
// being set, and tests/unit/test_db_exec.cc just GTEST_SKIP()'s when
// EVGRPC_TEST_DATABASE_URL isn't set. The config.json path was
// implicit and unverified — only proven by manual runs of
// `./build/src/evgrpc_server`.
//
// These tests verify the four code paths: missing file, valid JSON,
// missing keys, malformed JSON. The happy path is also exercised
// end-to-end by the integration test run with no env vars (see the
// commit that introduces this file).

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "fixtures/pg_container.h"

namespace {

std::string WriteTempJson(const std::string& content) {
  char path[] = "/tmp/evgrpc_test_pgcfg_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("mkstemp failed");
  close(fd);
  std::ofstream f(path);
  f << content;
  f.close();
  return std::string(path);
}

std::string WriteTempText(const std::string& content) {
  char path[] = "/tmp/evgrpc_test_pgcfg_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("mkstemp failed");
  close(fd);
  std::ofstream f(path);
  f << content;
  f.close();
  return std::string(path);
}

}  // namespace

TEST(PgContainerConfigTest, MissingFileReturnsNullopt) {
  // /tmp/nonexistent-evgrpc-config-<random>.json never exists; if
  // it does, we have bigger problems.
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(
      "/tmp/nonexistent-evgrpc-config-12345.json");
  EXPECT_FALSE(result.has_value());
}

TEST(PgContainerConfigTest, ValidConfigReturnsUrl) {
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u:***@h:1/d" },
    "oauth": { "issuer_url": "https://x", "audience": "y" }
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "postgresql://u:***@h:1/d");
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, EmptyUrlReturnsNullopt) {
  // Empty database.url is treated as "not provided" so PgContainer
  // falls through to the EVGRPC_TEST_DB_PASSWORD branch.
  auto path = WriteTempJson(R"({
    "database": { "url": "" }
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, MissingDatabaseSectionReturnsNullopt) {
  auto path = WriteTempJson(R"({
    "oauth": { "issuer_url": "https://x", "audience": "y" }
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, MissingUrlFieldReturnsNullopt) {
  // database section present but no url field.
  auto path = WriteTempJson(R"({
    "database": { "host": "127.0.0.1", "port": 5432 }
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, DatabaseNotObjectReturnsNullopt) {
  // database is a string instead of an object.
  auto path = WriteTempJson(R"({
    "database": "not-an-object"
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, MalformedJsonReturnsNullopt) {
  // Trailing comma — invalid JSON.
  auto path = WriteTempJson(R"({
    "database": { "url": "postgresql://u@h/d", },
  })");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

TEST(PgContainerConfigTest, NonJsonFileReturnsNullopt) {
  // File with random text content.
  auto path = WriteTempText("this is not json at all\n");
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig(path);
  EXPECT_FALSE(result.has_value());
  std::remove(path.c_str());
}

// Integration-style smoke: when ./config.json exists at the path
// PgContainer reads (CWD-relative), the function returns the URL.
// Run this from the repo root to verify the actual config.json works.
TEST(PgContainerConfigTest, RepoRootConfigJsonIsReadable) {
  // Use the repo's actual config.json by spawning in CWD; this test
  // only validates the helper when CWD contains the file.
  // We don't want the test to fail if run from a different CWD,
  // so this is informational (not a hard assertion).
  auto result = evgrpc::test::ReadDatabaseUrlFromConfig("./config.json");
  if (result.has_value()) {
    // Sanity: URL starts with postgresql://
    EXPECT_EQ(result->substr(0, 13), "postgresql://")
        << "config.json database.url doesn't look like a postgresql URL: "
        << *result;
  } else {
    GTEST_SKIP() << "./config.json not present in CWD (test runner CWD = "
                 << std::string(getcwd(nullptr, 0) ? getcwd(nullptr, 0) : "?")
                 << ") — informational only, not a failure";
  }
}
