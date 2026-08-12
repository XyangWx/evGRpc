#pragma once
#include "config/config_loader.h"  // for LogConfig
#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace evgrpc::log {

// Initialize the global logging system from a LogConfig. Call once at
// startup, before any other code logs. Safe to call multiple times —
// each call clears and re-creates the registry (intended for tests;
// in production `main.cc` calls it exactly once).
//
// Sinks:
//   - stdout color sink: receives >= cfg.level (auto-color if TTY)
//   - stderr color sink: receives >= err (error|critical only)
//   - rotating file sink: receives >= cfg.level (only if cfg.file set);
//                         rotates at cfg.max_size_mb MB, keeps
//                         cfg.max_files old files
//
// Throws std::runtime_error if cfg.file is non-empty and its parent
// directory does not exist or is not writable (fail-fast).
//
// Named loggers (all initialized to cfg.level):
//   auth, service, db, jwks, server.
void Init(const evgrpc::LogConfig& cfg);

// Initialize logging with safe defaults (stderr + stdout sinks, level
// = info, no file). Use this BEFORE parsing config.json so that early
// errors (e.g. OIDC discovery failure during LoadConfig) still land
// somewhere visible. Call Init(cfg.log) afterward to upgrade with the
// resolved log.file / log.level from config.json.
void InitDefaults();

// Look up a named logger. Lazy-creates if not registered.
std::shared_ptr<spdlog::logger> Get(const std::string& name);

// Re-apply level at runtime.
void SetLevel(spdlog::level::level_enum level);

}  // namespace evgrpc::log