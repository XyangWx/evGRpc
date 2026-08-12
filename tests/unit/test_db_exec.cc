#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include "config/config_loader.h"
#include "db/exec.h"
#include "db/pool.h"
#include "fixtures/pg_container.h"
#include "log/log.h"
#include <spdlog/spdlog.h>

namespace {

std::string TestDatabaseUrl() {
  // Prefer explicit env var (CI / per-test override).
  if (const char* url = std::getenv("EVGRPC_TEST_DATABASE_URL"); url && *url) {
    return url;
  }
  // Fallback to repo-root ./config.json — same precedence as PgContainer.
  if (auto url = evgrpc::test::ReadDatabaseUrlFromConfig("./config.json")) {
    return *url;
  }
  return "";
}

std::string ReadAll(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST(DbExecTest, SuccessEmitsDebugLines) {
  auto url = TestDatabaseUrl();
  if (url.empty()) {
    GTEST_SKIP() << "EVGRPC_TEST_DATABASE_URL not set (and no ./config.json fallback)";
  }
  const std::string log_path = "/tmp/evgrpc_test_db_exec_success.log";
  std::remove(log_path.c_str());

  evgrpc::LogConfig lc;
  lc.level = "debug";
  lc.file = log_path;
  lc.max_size_mb = 1;
  lc.max_files = 1;
  evgrpc::log::Init(lc);

  evgrpc::PgPool pool(url, /*size=*/1);
  auto c = pool.acquire();
  pqxx::work tx(*c);
  auto r = evgrpc::db::Exec(tx, "SELECT 1::int AS n", "test.selectOne");
  EXPECT_EQ(r.size(), 1u);

  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });
  auto content = ReadAll(log_path);
  EXPECT_NE(content.find("label=test.selectOne"), std::string::npos) << content;
  EXPECT_NE(content.find("stmt=\"SELECT 1::int AS n\""), std::string::npos) << content;
  EXPECT_NE(content.find("elapsed_us="), std::string::npos) << content;
}

TEST(DbExecTest, NoArgsPath) {
  auto url = TestDatabaseUrl();
  if (url.empty()) {
    GTEST_SKIP() << "EVGRPC_TEST_DATABASE_URL not set (and no ./config.json fallback)";
  }
  evgrpc::PgPool pool(url, /*size=*/1);
  auto c = pool.acquire();
  pqxx::work tx(*c);
  // 0-arg call must compile and execute.
  auto r = evgrpc::db::Exec(tx, "SELECT now()", "diag.now");
  EXPECT_EQ(r.size(), 1u);
}

TEST(DbExecTest, FailureEmitsWarn) {
  auto url = TestDatabaseUrl();
  if (url.empty()) {
    GTEST_SKIP() << "EVGRPC_TEST_DATABASE_URL not set (and no ./config.json fallback)";
  }
  const std::string log_path = "/tmp/evgrpc_test_db_exec_fail.log";
  std::remove(log_path.c_str());

  evgrpc::LogConfig lc;
  lc.level = "info";  // intentionally higher than debug
  lc.file = log_path;
  lc.max_size_mb = 1;
  lc.max_files = 1;
  evgrpc::log::Init(lc);

  evgrpc::PgPool pool(url, /*size=*/1);
  auto c = pool.acquire();
  pqxx::work tx(*c);
  // Deliberately broken SQL.
  try {
    evgrpc::db::Exec(tx, "SELECT * FROM no_such_table_xyz", "test.fail");
    FAIL() << "expected pqxx::sql_error";
  } catch (const pqxx::sql_error&) {
    // expected
  }

  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });
  auto content = ReadAll(log_path);
  EXPECT_NE(content.find("FAILED"), std::string::npos) << content;
  EXPECT_NE(content.find("label=test.fail"), std::string::npos) << content;
  EXPECT_NE(content.find("errcode="), std::string::npos) << content;
}

TEST(DbExecTest, SilentAtInfoLevel) {
  auto url = TestDatabaseUrl();
  if (url.empty()) {
    GTEST_SKIP() << "EVGRPC_TEST_DATABASE_URL not set (and no ./config.json fallback)";
  }
  const std::string log_path = "/tmp/evgrpc_test_db_exec_silent.log";
  std::remove(log_path.c_str());

  evgrpc::LogConfig lc;
  lc.level = "info";
  lc.file = log_path;
  lc.max_size_mb = 1;
  lc.max_files = 1;
  evgrpc::log::Init(lc);

  evgrpc::PgPool pool(url, /*size=*/1);
  auto c = pool.acquire();
  pqxx::work tx(*c);
  auto r = evgrpc::db::Exec(tx, "SELECT 42", "test.silent");
  EXPECT_EQ(r.size(), 1u);

  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });
  auto content = ReadAll(log_path);
  // No debug lines should appear at info level.
  EXPECT_EQ(content.find("test.silent"), std::string::npos)
      << "expected no debug line at info level; got:\n" << content;
}
