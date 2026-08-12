#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "config/config_loader.h"
#include "log/log.h"
#include <spdlog/spdlog.h>

namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

evgrpc::LogConfig DefaultLogConfig() {
  evgrpc::LogConfig c;
  c.level = "info";
  c.file = "";
  c.max_size_mb = 100;
  c.max_files = 7;
  return c;
}

}  // namespace

TEST(LogInitTest, IdempotentWhenUnchanged) {
  evgrpc::log::Init(DefaultLogConfig());
  evgrpc::log::Init(DefaultLogConfig());
  SUCCEED();
}

TEST(LogInitTest, RespectsLogLevelDebug) {
  auto cfg = DefaultLogConfig();
  cfg.level = "debug";
  const std::string path = "./log/test_log_level_debug.log";
  cfg.file = path;
  std::remove(path.c_str());
  evgrpc::log::Init(cfg);

  auto auth = evgrpc::log::Get("auth");
  auth->debug("debug-visible");
  auth->info("info-visible");
  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });

  auto content = ReadAll(path);
  EXPECT_NE(content.find("debug-visible"), std::string::npos)
      << "expected debug line in log file; got:\n" << content;
  EXPECT_NE(content.find("info-visible"), std::string::npos);
}

TEST(LogInitTest, StderrSinkOnlyReceivesErrorOrAbove) {
  auto cfg = DefaultLogConfig();
  cfg.level = "trace";
  const std::string path = "./log/test_log_stderr_filter.log";
  cfg.file = path;
  std::remove(path.c_str());
  evgrpc::log::Init(cfg);

  auto l = evgrpc::log::Get("server");
  l->info("info-to-stdout");
  l->error("error-to-stderr-and-file");
  l->critical("critical-to-stderr-and-file");
  spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->flush(); });

  auto content = ReadAll(path);
  EXPECT_NE(content.find("info-to-stdout"), std::string::npos);
  EXPECT_NE(content.find("error-to-stderr-and-file"), std::string::npos);
  EXPECT_NE(content.find("critical-to-stderr-and-file"), std::string::npos);
}

TEST(LogInitTest, GetReturnsSameLoggerForSameName) {
  evgrpc::log::Init(DefaultLogConfig());
  auto a1 = evgrpc::log::Get("auth");
  auto a2 = evgrpc::log::Get("auth");
  EXPECT_EQ(a1.get(), a2.get());

  auto b = evgrpc::log::Get("db");
  EXPECT_NE(a1.get(), b.get());
}

TEST(LogInitTest, SetLevelAppliesToAllLoggers) {
  auto cfg = DefaultLogConfig();
  cfg.level = "info";
  evgrpc::log::Init(cfg);
  auto auth = evgrpc::log::Get("auth");
  EXPECT_EQ(auth->level(), spdlog::level::info);

  evgrpc::log::SetLevel(spdlog::level::debug);
  EXPECT_EQ(auth->level(), spdlog::level::debug);
}

TEST(LogInitTest, ThrowsOnUnwritableLogFileParent) {
  auto cfg = DefaultLogConfig();
  cfg.file = "/nonexistent/dir/that/does/not/exist/x.log";
  EXPECT_THROW(evgrpc::log::Init(cfg), std::runtime_error);
}