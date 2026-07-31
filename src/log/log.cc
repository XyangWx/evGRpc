#include "log/log.h"

#include <cstdlib>
#include <iostream>
#include <vector>
#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace evgrpc::log {

namespace {

spdlog::level::level_enum ParseLevel(const char* s) {
  using namespace spdlog::level;
  if (!s) return info;
  std::string str(s);
  if (str == "trace") return trace;
  if (str == "debug") return debug;
  if (str == "info")  return info;
  if (str == "warn" || str == "warning") return warn;
  if (str == "error" || str == "err") return err;
  if (str == "critical" || str == "crit") return critical;
  return info;
}

const char* GetEnvOr(const char* var, const char* fallback) {
  const char* v = std::getenv(var);
  return (v && *v) ? v : fallback;
}

// Text pattern shared by all sinks. The `%^...%$` markers enable color
// only when the destination is a TTY (spdlog's color sinks handle this
// automatically — the markers are no-ops when not a TTY).
constexpr char kTextPattern[] =
    "[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$] [%n] %v";

}  // namespace

void Init() {
  spdlog::drop_all();  // idempotent re-init: clear and rebuild

  auto level = ParseLevel(GetEnvOr("LOG_LEVEL", "info"));
  bool want_json = std::string(GetEnvOr("LOG_FORMAT", "text")) == "json";
  if (want_json) {
    std::cerr << "[evgrpc-log] LOG_FORMAT=json is reserved for v2; "
              << "falling back to text" << std::endl;
    want_json = false;
  }

  auto stdout_sink =
      std::make_shared<spdlog::sinks::ansicolor_stdout_sink_st>();
  stdout_sink->set_level(level);
  stdout_sink->set_pattern(kTextPattern);

  auto stderr_sink =
      std::make_shared<spdlog::sinks::ansicolor_stderr_sink_st>();
  stderr_sink->set_level(spdlog::level::err);
  stderr_sink->set_pattern(kTextPattern);

  std::vector<spdlog::sink_ptr> sinks{stdout_sink, stderr_sink};

  const char* log_file = GetEnvOr("LOG_FILE", "");
  if (*log_file) {
    int max_size_mb = std::atoi(GetEnvOr("LOG_FILE_MAX_SIZE_MB", "100"));
    int max_files = std::atoi(GetEnvOr("LOG_FILE_MAX_FILES", "7"));
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_st>(
        log_file,
        /*max_size=*/static_cast<size_t>(max_size_mb) * 1024 * 1024,
        /*max_files=*/max_files);
    file_sink->set_level(level);
    file_sink->set_pattern(kTextPattern);
    sinks.push_back(file_sink);
  }

  for (const char* name : {"auth", "service", "db", "jwks", "server"}) {
    auto logger = std::make_shared<spdlog::logger>(
        name, sinks.begin(), sinks.end());
    logger->set_level(level);
    spdlog::register_logger(logger);
  }
}

std::shared_ptr<spdlog::logger> Get(const std::string& name) {
  auto existing = spdlog::get(name);
  if (existing) return existing;
  // Fallback: tests may call Get() before Init(). Build a fresh logger
  // so they don't NPE. Production code calls Init() at startup so this
  // path is unreachable.
  auto logger = std::make_shared<spdlog::logger>(name);
  spdlog::register_logger(logger);
  return logger;
}

void SetLevel(spdlog::level::level_enum level) {
  spdlog::apply_all([level](std::shared_ptr<spdlog::logger> l) {
    l->set_level(level);
  });
}

}  // namespace evgrpc::log