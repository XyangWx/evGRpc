#include <cstdlib>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include "config/config_loader.h"
#include "db/pool.h"
#include "log/log.h"
#include <spdlog/spdlog.h>

namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

std::string TestDatabaseUrl() {
  const char* url = std::getenv("EVGRPC_TEST_DATABASE_URL");
  return url ? url : "";
}

}  // namespace

TEST(PoolEventsTest, AcquireAndReleaseEmitDebugLines) {
  auto url = TestDatabaseUrl();
  if (url.empty()) {
    GTEST_SKIP() << "EVGRPC_TEST_DATABASE_URL not set";
  }

  const std::string log_path = "/tmp/evgrpc_test_pool_events.log";
  std::remove(log_path.c_str());

  evgrpc::LogConfig lc;
  lc.level = "debug";
  lc.file = log_path;
  lc.max_size_mb = 1;
  lc.max_files = 1;
  evgrpc::log::Init(lc);

  evgrpc::PgPool pool(url, /*size=*/2);
  {
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();   // forces wait or second-conn acquire
    // c1 and c2 destruct → release (must happen before flush+read below)
  }

  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });

  auto content = ReadAll(log_path);
  EXPECT_NE(content.find("pool.acquire"), std::string::npos)
      << "expected pool.acquire debug line; got:\n" << content;
  EXPECT_NE(content.find("pool.release"), std::string::npos)
      << "expected pool.release debug line; got:\n" << content;
}
