#include "log/log.h"

#include <iostream>
#include <vector>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace evgrpc::log {

namespace {

spdlog::level::level_enum ParseLevel(const std::string& s) {
  using namespace spdlog::level;
  if (s == "trace") return trace;
  if (s == "debug") return debug;
  if (s == "info")  return info;
  if (s == "warn" || s == "warning") return warn;
  if (s == "error" || s == "err") return err;
  if (s == "critical" || s == "crit") return critical;
  return info;
}

constexpr char kTextPattern[] =
    "[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$] [%n] %v";

bool IsWritableDir(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  if (!S_ISDIR(st.st_mode)) return false;
  return access(path.c_str(), W_OK) == 0;
}

}  // namespace

void Init(const evgrpc::LogConfig& cfg) {
  spdlog::drop_all();

  auto level = ParseLevel(cfg.level);

  auto stdout_sink =
      std::make_shared<spdlog::sinks::ansicolor_stdout_sink_st>();
  stdout_sink->set_level(level);
  stdout_sink->set_pattern(kTextPattern);

  auto stderr_sink =
      std::make_shared<spdlog::sinks::ansicolor_stderr_sink_st>();
  stderr_sink->set_level(spdlog::level::err);
  stderr_sink->set_pattern(kTextPattern);

  std::vector<spdlog::sink_ptr> sinks{stdout_sink, stderr_sink};

  if (!cfg.file.empty()) {
    std::filesystem::path p(cfg.file);
    auto parent = p.parent_path();
    if (parent.empty()) parent = ".";
    if (!IsWritableDir(parent.string())) {
      throw std::runtime_error(
          "log.file: parent directory does not exist or is not writable: " +
          parent.string());
    }
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_st>(
        cfg.file,
        static_cast<size_t>(cfg.max_size_mb) * 1024 * 1024,
        cfg.max_files);
    file_sink->set_level(level);
    file_sink->set_pattern(kTextPattern);
    sinks.push_back(file_sink);
  }

  for (const char* name : {"auth", "service", "db", "jwks", "server"}) {
    auto logger = std::make_shared<spdlog::logger>(
        name, sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::err);
    spdlog::register_logger(logger);
  }
}

std::shared_ptr<spdlog::logger> Get(const std::string& name) {
  auto existing = spdlog::get(name);
  if (existing) return existing;
  auto logger = std::make_shared<spdlog::logger>(name);
  spdlog::register_logger(logger);
  return logger;
}

void SetLevel(spdlog::level::level_enum level) {
  spdlog::apply_all([level](std::shared_ptr<spdlog::logger> l) {
    l->set_level(level);
  });
}

void InitDefaults() {
  LogConfig defaults;
  defaults.level = "info";
  defaults.file = "";        // no file sink yet
  defaults.max_size_mb = 100;
  defaults.max_files = 7;
  Init(defaults);
}

}  // namespace evgrpc::log