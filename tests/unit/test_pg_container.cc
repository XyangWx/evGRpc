#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

#include "fixtures/pg_container.h"

namespace evgrpc::test {
namespace {

// Tests exercise the URL/keyword parser via the public PgContainer
// constructor (which doesn't open a connection — fields are populated
// synchronously from env, no DB needed). Each test sets/un-sets env in
// SetUp/TearDown to avoid cross-test pollution — env vars are
// process-global.

class PgContainerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    unsetenv("DATABASE_URL");
    unsetenv("EVGRPC_TEST_DB_PASSWORD");
    unsetenv("EVGRPC_TEST_DB_URL_OVERRIDE");  // sentinel below
  }
  void TearDown() override {
    unsetenv("DATABASE_URL");
    unsetenv("EVGRPC_TEST_DB_PASSWORD");
    unsetenv("EVGRPC_TEST_DB_URL_OVERRIDE");
  }
  // The fallback path also tries ./config.json (production binary's
  // default config path). To test the env-var-only fallback
  // (FallbackUsesDefaults / MissingBothEnvVarsMentionsBoth), we
  // temporarily redirect PgContainer to a non-existent config file
  // via chdir. Cleaner alternative: rename config.json during the
  // test, but that's racy in parallel. chdir+restore is per-test.
};

// --- DATABASE_URL (URI form) ---

// All 5 fields populated from a URI with explicit port.
TEST_F(PgContainerTest, ParsesUrlFormFull) {
  setenv("DATABASE_URL", "postgresql://u:p@h:1234/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Host(), "h");
  EXPECT_EQ(pg.Port(), 1234);
  EXPECT_EQ(pg.Database(), "db");
  EXPECT_EQ(pg.Username(), "u");
  EXPECT_EQ(pg.Password(), "p");
}

// No explicit port — caller defaults to 5432 (mirrors libpq behavior).
TEST_F(PgContainerTest, ParsesUrlFormDefaultPort) {
  setenv("DATABASE_URL", "postgresql://u:p@h/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Host(), "h");
  EXPECT_EQ(pg.Port(), 5432);
  EXPECT_EQ(pg.Database(), "db");
}

// User only, no password. Common for trust-auth local dev.
TEST_F(PgContainerTest, ParsesUrlFormNoPassword) {
  setenv("DATABASE_URL", "postgresql://u@h/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Username(), "u");
  EXPECT_EQ(pg.Password(), "");
}

// URL-encoded password — `@` (which would otherwise be parsed as
// userinfo/host separator) must be escaped as `%40` per RFC 3986.
TEST_F(PgContainerTest, ParsesUrlFormEncodedPassword) {
  setenv("DATABASE_URL", "postgresql://u:p%40ss@h/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Password(), "p@ss");
}

// 'postgres://' (no 'ql') scheme — libpq accepts both.
TEST_F(PgContainerTest, ParsesUrlFormPostgresScheme) {
  setenv("DATABASE_URL", "postgres://u:p@h/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Host(), "h");
  EXPECT_EQ(pg.Username(), "u");
}

// Query string is dropped after dbname. Useful for sslmode=disable,
// application_name, etc.
TEST_F(PgContainerTest, ParsesUrlFormWithQueryString) {
  setenv("DATABASE_URL", "postgresql://u:p@h:1/db?sslmode=disable", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Database(), "db");
  EXPECT_EQ(pg.Port(), 1);
}

// Raw URL is preserved in conninfo_ so libpqxx can consume it
// unmodified (libpq's URL parser is the one we delegate to at connect
// time; we don't round-trip through the per-field accessors).
TEST_F(PgContainerTest, UrlFormConninfoPreserved) {
  setenv("DATABASE_URL", "postgresql://u:p%40ss@h:1234/db", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Conninfo(), "postgresql://u:p%40ss@h:1234/db");
}

// --- DATABASE_URL (keyword form — regression) ---

// libpq keyword form must keep working — some scripts/CI tools
// prefer it for env files (no URL-encoding headache for passwords).
TEST_F(PgContainerTest, ParsesKeywordForm) {
  setenv("DATABASE_URL",
         "host=h port=1234 dbname=db user=u password=p", 1);
  PgContainer pg;
  EXPECT_EQ(pg.Host(), "h");
  EXPECT_EQ(pg.Port(), 1234);
  EXPECT_EQ(pg.Database(), "db");
  EXPECT_EQ(pg.Username(), "u");
  EXPECT_EQ(pg.Password(), "p");
}

// --- Fallback path (no DATABASE_URL) ---

// When DATABASE_URL is unset AND no ./config.json fallback is
// reachable, EVGRPC_TEST_DB_PASSWORD + hardcoded defaults wire up
// the local-dev connection. Test runs from a temp CWD so the
// ./config.json fallback path doesn't shadow this branch.
TEST_F(PgContainerTest, FallbackUsesDefaults) {
  setenv("EVGRPC_TEST_DB_PASSWORD", "pw", 1);
  // Run from /tmp so ./config.json is unreachable from PgContainer.
  char saved_cwd[4096];
  getcwd(saved_cwd, sizeof(saved_cwd));
  chdir("/tmp");
  PgContainer pg;
  chdir(saved_cwd);
  EXPECT_EQ(pg.Host(), "127.0.0.1");
  EXPECT_EQ(pg.Port(), 5432);
  EXPECT_EQ(pg.Database(), "evgrpc");
  EXPECT_EQ(pg.Username(), "evgrpc_admin");
  EXPECT_EQ(pg.Password(), "pw");
  EXPECT_NE(pg.Conninfo().find("host=127.0.0.1"), std::string::npos);
  EXPECT_NE(pg.Conninfo().find("password=pw"), std::string::npos);
}

// --- Error paths ---

// Missing user/dbname/host surfaces a clear error rather than silently
// falling through to the (different) EVGRPC_TEST_DB_PASSWORD fallback.
TEST_F(PgContainerTest, MalformedUrlThrows) {
  setenv("DATABASE_URL", "postgresql://only_host_no_user_or_db", 1);
  EXPECT_THROW(PgContainer pg, std::runtime_error);
}

// Neither DATABASE_URL nor EVGRPC_TEST_DB_PASSWORD (and no
// ./config.json fallback reachable) → error message mentions both
// env-var options. Same chdir-to-/tmp trick to skip config.json.
TEST_F(PgContainerTest, MissingBothEnvVarsMentionsBoth) {
  char saved_cwd[4096];
  getcwd(saved_cwd, sizeof(saved_cwd));
  chdir("/tmp");
  try {
    PgContainer pg;
    FAIL() << "expected throw";
  } catch (const std::runtime_error& e) {
    chdir(saved_cwd);
    std::string msg = e.what();
    EXPECT_NE(msg.find("EVGRPC_TEST_DB_PASSWORD"), std::string::npos)
        << "should mention EVGRPC_TEST_DB_PASSWORD: " << msg;
    EXPECT_NE(msg.find("DATABASE_URL"), std::string::npos)
        << "should mention DATABASE_URL: " << msg;
    return;
  } catch (...) {
    chdir(saved_cwd);
    throw;
  }
  chdir(saved_cwd);
}

// --- config.json fallback (new in this commit) ---

// With neither DATABASE_URL nor EVGRPC_TEST_DB_PASSWORD set but a
// readable ./config.json present, PgContainer should pick up
// database.url from the file. Verifies the new fallback chain.
TEST_F(PgContainerTest, ConfigJsonFallback) {
  // This test relies on the repo's config.json being readable from
  // the runner's CWD. We assume tests run from repo root
  // (CMake's WORKING_DIRECTORY is set there). Skip if not.
  if (access("./config.json", R_OK) != 0) {
    GTEST_SKIP() << "./config.json not reachable from CWD " << getcwd(nullptr, 0);
  }
  PgContainer pg;
  // The actual URL in config.json starts with postgresql://; verify
  // by parsing the URL portion.
  EXPECT_EQ(pg.Conninfo().substr(0, 13), "postgresql://")
      << "config.json fallback didn't yield a postgresql URL; "
      << "got Conninfo = " << pg.Conninfo();
}

}  // namespace
}  // namespace evgrpc::test
