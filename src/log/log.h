#pragma once
#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace evgrpc::log {

// Initialize the global logging system. Call once at startup, before any
// other code logs. Safe to call multiple times — each call clears and
// re-creates the registry from current env vars (intended for tests; in
// production `main.cc` calls it exactly once).
//
// Env vars (read at every Init()):
//   LOG_LEVEL              info|trace|debug|warn|error|critical  (default: info)
//   LOG_FORMAT             text|json                              (default: text;
//                                                              `json` rejected w/ warn)
//   LOG_FILE               /path/to/file                          (default: empty;
//                                                              empty = no file sink)
//   LOG_FILE_MAX_SIZE_MB   int                                   (default: 100)
//   LOG_FILE_MAX_FILES     int                                   (default: 7)
//
// Sinks:
//   - stdout color sink: receives ≥ LOG_LEVEL (auto-color if TTY)
//   - stderr color sink: receives ≥ `err` (error|critical only)
//   - rotating file sink: receives ≥ LOG_LEVEL (only if LOG_FILE set);
//                         rotates at LOG_FILE_MAX_SIZE_MB MB, keeps
//                         LOG_FILE_MAX_FILES old files
//
// Named loggers (all initialized to LOG_LEVEL):
//   auth, service, db, jwks, server  — see spec §5.6 for ownership.
void Init();

// Look up a named logger. Lazy-creates if not registered (returns a fresh
// logger with default level — silent fallback for tests; production code
// should rely on Init() having been called).
std::shared_ptr<spdlog::logger> Get(const std::string& name);

// Re-apply level at runtime (e.g., from a SIGHUP handler in Task 15).
void SetLevel(spdlog::level::level_enum level);

}  // namespace evgrpc::log