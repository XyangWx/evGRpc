# evGRpc — config.json migration

**Date:** 2026-08-06
**Status:** Draft — pending user review
**Repo:** `/workspace/repositories/evGRpc`
**Supersedes:** env-var runtime config from `docs/superpowers/specs/2026-07-30-evgrpc-design.md` §9.3 (Runtime Config) and §5.4 (Configuration) only.

---

## 1. Goals & Scope

### Goal

Move all runtime configuration from environment variables to a single `config.json` file, and add **Debug-level SQL statement logging** so DB operations are inspectable in development and post-mortem.

### Why now

The current env-var approach (6 `OAUTH_*` / `DATABASE_URL` / `LOG_*` vars) is friction in three places:

1. **Docker deployment** — every config change requires a container restart with a new env-var set; no versioned, file-based audit trail.
2. **SQL debugging** — `db` named logger exists but does not log statements; the only way to see what the server executed against PostgreSQL is to enable PostgreSQL's `log_statement=all` on the DB side, which is noisy and out of scope for the application layer.
3. **Operational visibility** — config is scattered across shell exports / container env / systemd unit files; one JSON file is one source of truth.

### In Scope

- Replace 9 environment variables with one `config.json` (4 nested sections: `database` / `oauth` / `grpc` / `log`).
- Auto-derive `oauth.jwks_url` from `oauth.issuer_url` via OIDC discovery (eager at startup, cached for process lifetime, fail-fast).
- Hand-written schema validation with clear field-path errors.
- Strict JSON (RFC 8259) — no JSON5 / JSONC / comments.
- New `evgrpc::db::Exec()` wrapper that logs every SQL statement at `db.debug` (statement + param count + affected rows + elapsed microseconds) and at `db.warn` on `pqxx::sql_error` (with errcode + errmsg).
- New `pool.acquire` / `pool.release` debug events on `PgPool`.
- `--config <path>` / `-c <path>` cmdline flag with `./config.json` default.
- `--help` / `-h` flag printing usage to stdout.
- `config.example.json` at repo root for ops/dev reference.

### Out of Scope

- TLS for the gRPC server (still `InsecureServerCredentials`).
- `/healthz` / `/readyz` health checks.
- Prometheus `/metrics`.
- Hot-reload of `config.json` (SIGHUP does not re-read).
- Multi-database support (PostgreSQL only via `pqxx`).
- JSON5 / JSONC / comments in `config.json`.
- Per-field environment variable override of `config.json` values.
- Per-RPC or per-service log levels.
- Log file rotation by date (still by size).
- Schema versioning field (`version: 1`) — single version assumed.
- Connection pool size configuration (still hardcoded to 4 in `PgPool` constructor).
- OIDC discovery doc refresh (cached for process lifetime, not re-fetched on TTL).

---

## 2. Configuration File

### 2.1 Path & Discovery

- **Default:** `./config.json` (relative to process CWD).
- **Override:** `--config <path>` or `-c <path>` cmdline flag.
- **No file at default path and no flag:** process prints `[evgrpc-config] config.json: cannot open: <reason>` to stderr and exits with code `1`.
- **`--help` / `-h`:** prints usage to stdout, exits `0` without touching config.

### 2.2 Schema (canonical)

```json
{
  "database": {
    "url": "postgresql://user:password@host:5432/dbname"
  },
  "oauth": {
    "issuer_url": "https://auth.example.com/",
    "audience": "evgrpc-api",
    "jwks_cache_ttl_seconds": 3600
  },
  "grpc": {
    "port": 50051
  },
  "log": {
    "level": "info",
    "file": "/var/log/evgrpc/evgrpc.log",
    "max_size_mb": 100,
    "max_files": 7
  }
}
```

### 2.3 Field Reference

| Path | Type | Required | Default | Constraints |
|---|---|---|---|---|
| `database` | object | yes | — | only `url` allowed |
| `database.url` | string | yes | — | non-empty; starts with `postgresql://` |
| `oauth` | object | yes | — | only `issuer_url` / `audience` / `jwks_cache_ttl_seconds` allowed |
| `oauth.issuer_url` | string | yes | — | non-empty; valid `http://` or `https://` URL; no trailing-slash normalization issues (see §3.2) |
| `oauth.audience` | string | yes | — | non-empty |
| `oauth.jwks_cache_ttl_seconds` | integer | no | `3600` | `> 0` and `≤ 86400` |
| `grpc` | object | yes | — | only `port` allowed |
| `grpc.port` | integer | no | `50051` | `1 ≤ x ≤ 65535` |
| `log` | object | yes | — | only the 4 keys below allowed |
| `log.level` | string | no | `"info"` | one of `trace` / `debug` / `info` / `warn` / `error` / `critical` |
| `log.file` | string | no | `""` | empty = no file sink; non-empty = parent directory must exist and be writable |
| `log.max_size_mb` | integer | no | `100` | `> 0` and `≤ 1024` |
| `log.max_files` | integer | no | `7` | `> 0` and `≤ 100` |

### 2.4 Validation Errors

Validation collects **all** field errors before returning (not fail-fast on the first), so an operator sees every misconfiguration in one pass. Errors are written to stderr, one per line, in the form:

```
<config_path>: <field_path>: <message> (got <value>)
```

Examples:

```
./config.json: database.url: missing required field
./config.json: oauth.issuer_url: not a valid http(s) URL (got "auth.example.com")
./config.json: oauth.jwks_cache_ttl_seconds: must be > 0 (got -5)
./config.json: log.level: must be one of trace/debug/info/warn/error/critical (got "verbose")
./config.json: log.file: parent directory does not exist or is not writable (got "/var/log/evgrpc")
./config.json: oauth: unknown key "tenant_id" (allowed: issuer_url, audience, jwks_cache_ttl_seconds)
./config.json: parse error at line 7 column 3: expected '}' or ','
```

Process exits with code `1` after printing all errors.

### 2.5 What Is Not in the File

- `database.pool_size` — pool size is hardcoded to `4` in `PgPool` constructor (current behavior preserved).
- `oauth.jwks_url` — derived at startup via OIDC discovery (§3.2), not user-configurable.
- `log.format` — removed; text pattern is the only format. The previous `LOG_FORMAT=json` env var was a "reserved for v2" placeholder and is now deleted.
- No `version` field — single schema version assumed.

---

## 3. Module Architecture

### 3.1 New Modules

#### `src/config/config_loader.{h,cc}`

```cpp
namespace evgrpc {

struct DatabaseConfig { std::string url; };
struct OAuthConfig    { std::string issuer_url; std::string audience;
                         int jwks_cache_ttl_seconds; };
struct GrpcConfig     { int port; };
struct LogConfig      { std::string level; std::string file;
                         int max_size_mb; int max_files; };

struct SchemaConfig {
    DatabaseConfig database;
    OAuthConfig    oauth;
    GrpcConfig     grpc;
    LogConfig      log;
};

// Reads <path>, parses JSON, validates every field, returns SchemaConfig.
// Throws std::runtime_error whose .what() joins all validation errors
// with '\n' separators, suitable for direct stderr output.
SchemaConfig LoadSchema(const std::string& path);

}  // namespace evgrpc
```

Pure file I/O + JSON parse + validation. No network. No side effects beyond reading the file.

#### `src/auth/oidc_discovery.{h,cc}`

```cpp
namespace evgrpc::auth {

struct OidcDiscoveryConfig {
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(2)};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(5)};
};

// GET {issuer_url}/.well-known/openid-configuration, parse JSON,
// return discovery["jwks_uri"]. Throws std::runtime_error on:
//   - issuer_url not http(s)://
//   - non-2xx HTTP response
//   - connect or read timeout
//   - response body not valid JSON
//   - missing 'jwks_uri' field
//   - 'jwks_uri' is not http(s)://
//
// URL construction: trim trailing '/' from issuer_url, then append
// "/.well-known/openid-configuration". Examples:
//   "https://auth.example.com"        -> "https://auth.example.com/.well-known/openid-configuration"
//   "https://auth.example.com/"       -> same
//   "https://auth.example.com/tenant" -> "https://auth.example.com/tenant/.well-known/openid-configuration"
std::string DiscoverJwksUri(const std::string& issuer_url,
                            const OidcDiscoveryConfig& cfg = {});

}  // namespace evgrpc::auth
```

Uses `cpp_httplib` (already in `cmake/deps.cmake` via `FetchContent_Declare(cpp_httplib ...)`).

#### `src/db/exec.{h,cc}`

```cpp
namespace evgrpc::db {

// Variadic: forwards args to tx.exec_params(sql, args...).
// On success: emits 2 debug lines (statement, then result) and returns
// the pqxx::result. On pqxx::sql_error: emits 1 warn line with errcode
// + errmsg + elapsed_us, then rethrows.
//
// Log level semantics: spdlog filters debug lines by global level.
// If log.level=info, the entire success path is silent. Failure
// path is always at warn (not gated by level).
template <typename... Args>
pqxx::result Exec(pqxx::transaction_base& tx,
                  std::string_view sql,
                  std::string_view label,    // e.g., "vehicle.create"
                  Args&&... args);

}  // namespace evgrpc::db
```

Parameter formatting (for debug log):
- `std::string` / `std::string_view` / `const char*` → truncated to 64 chars + `...` if longer
- integer types → decimal representation
- `std::optional<T>` → `null` or inner value
- `bool` → `true` / `false`
- all other types → `<typename>` placeholder

#### `src/util/args.{h,cc}`

```cpp
namespace evgrpc {

// Parses argv. Recognizes:
//   --config <path> | -c <path>  (sets config_path)
//   --help | -h                  (sets help_requested = true)
// On success: returns true, populates output args.
// On error (unknown flag, missing value, --help): returns false.
// Callers should print usage to stdout/stderr as appropriate.
struct ArgvResult {
    std::string config_path;  // default "./config.json"
    bool help_requested{false};
};
ArgvResult ParseArgs(int argc, char** argv);

}  // namespace evgrpc
```

### 3.2 Modified Modules

#### `src/config/config.{h,cc}` — split into Schema + Runtime

```cpp
namespace evgrpc {

// After OIDC discovery is resolved into a concrete jwks_url.
struct RuntimeConfig {
    DatabaseConfig database;
    struct {
        std::string issuer_url;
        std::string jwks_url;     // populated by OIDC discovery
        std::string audience;
        int jwks_cache_ttl_seconds;
    } oauth;
    GrpcConfig grpc;
    LogConfig log;
};

// Combined: LoadSchema(path) + DiscoverJwksUri(issuer_url) + assemble.
// Throws std::runtime_error on any failure (parse, validation, HTTP).
RuntimeConfig LoadConfig(const std::string& path);

}  // namespace evgrpc
```

The old `static Config Config::Load()` (env-var-based) is **deleted**. All env-var reads in `config.cc` are removed.

#### `src/log/log.{h,cc}` — accept `LogConfig` instead of env

Signature change:
```cpp
// OLD: void Init();
// NEW:
void Init(const evgrpc::LogConfig& cfg);
```

Behavior:
- `cfg.level` → `ParseLevel()` (existing helper, accepts trace/debug/info/warn/error/critical)
- `cfg.file` empty → no file sink; non-empty → rotating file sink with `cfg.max_size_mb` / `cfg.max_files` caps
- `cfg.file` non-empty but parent dir missing or not writable → `throw std::runtime_error`
- 5 named loggers (`auth`, `service`, `db`, `jwks`, `server`) registered at the configured level
- `flush_on(err)` on every logger preserved
- Text pattern `kTextPattern` preserved
- JSON format code path **deleted** (was a "reserved for v2" no-op that warned to stderr and fell back to text)
- All `LOG_LEVEL` / `LOG_FORMAT` / `LOG_FILE` / `LOG_FILE_MAX_SIZE_MB` / `LOG_FILE_MAX_FILES` reads from env are deleted

#### `src/db/pool.{h,cc}` — pool event logging

`pool.h` unchanged. `pool.cc` gains:
- `#include "log/log.h"`
- `acquire()` emits one `db.debug` line: `pool.acquire idle_remaining=<N> wait_us=<N>`
- `release()` emits one `db.debug` line: `pool.release idle_remaining=<N>`

Both gated on `log->should_log(spdlog::level::debug)` to avoid the `spdlog::get("db")` lookup cost when not debugging.

#### `src/services/*_service.cc` (5 files) — replace direct `tx.exec*` calls

Mechanical replacement: every `tx.exec_params(sql, ...)` and `tx.exec(sql)` becomes `evgrpc::db::Exec(tx, sql, "<service>.<verb>", ...)`. The label convention is `<service>.<verb>`, e.g.:

- `"vehicle.create"`
- `"vehicle.findById"`
- `"weather.findByLocation"`
- `"consumption.listByVehicle"`
- `"charging.create"`
- `"display.costByChargerType"`

`tx.exec_params0` / `tx.exec0` calls (0-arg) also become `db::Exec(tx, sql, label)` — the variadic template handles 0 args via the 0-arg `exec_params` overload.

#### `src/main.cc` — wire everything

```cpp
int main(int argc, char** argv) {
    auto args = evgrpc::ParseArgs(argc, argv);
    if (args.help_requested) {
        std::cout << "usage: evgrpc [--config <path>|-c <path>] [--help|-h]\n";
        return 0;
    }

    evgrpc::RuntimeConfig cfg;
    try {
        cfg = evgrpc::LoadConfig(args.config_path);
    } catch (const std::exception& e) {
        std::cerr << "[evgrpc-config] " << e.what() << std::endl;
        return 1;
    }

    evgrpc::log::Init(cfg.log);
    auto server_log = evgrpc::log::Get("server");
    server_log->info("config loaded from {}", args.config_path);

    evgrpc::PgPool pool(cfg.database.url);

    auto jwks = std::make_shared<evgrpc::JwksCache>(
        cfg.oauth.jwks_url,
        std::chrono::seconds(cfg.oauth.jwks_cache_ttl_seconds));
    auto validator = std::make_shared<evgrpc::JwtValidator>(evgrpc::JwtValidator{
        .issuer = cfg.oauth.issuer_url,
        .audience = cfg.oauth.audience,
        .resolve_key = [jwks](const std::string& kid) { return jwks->GetKey(kid); },
    });

    evgrpc::VehicleServiceImpl vehicle_svc(&pool, validator.get());
    // ... 4 more services ...
    auto server = builder.BuildAndStart();
    // ... signal handling unchanged ...
}
```

### 3.3 Removed Code

- All `getenv` calls in `src/config/config.cc` and `src/log/log.cc`.
- The `LOG_FORMAT=json` warning-and-fallback code path in `log.cc`.
- All `setenv("LOG_*", ...)` calls in test files.

---

## 4. SQL Debug Logging Details

### 4.1 Output Format

Per-query success path (two debug lines):

```
[2026-08-06 14:32:01.123 +0800] [debug] [db] sql label=vehicle.create stmt="INSERT INTO vehicle (...) VALUES (...)" params=3
[2026-08-06 14:32:01.124 +0800] [debug] [db] sql label=vehicle.create ok rows=1 elapsed_us=842
```

Per-query failure path (one warn line):

```
[2026-08-06 14:32:01.125 +0800] [warn] [db] sql label=vehicle.create FAILED stmt="INSERT INTO vehicle ..." errcode=23505 errmsg="duplicate key value violates unique constraint ..." elapsed_us=412
```

### 4.2 Visibility

- All SQL debug lines are at `spdlog::level::debug`.
- A single global `log.level` threshold controls visibility (no per-logger override beyond the existing 5 named loggers).
- When `log.level` is `info` or higher, SQL debug output is silent (no performance cost beyond the `should_log` check).
- SQL failure warn lines are **not** gated by level — they always emit, matching the existing `flush_on(err)` pattern.

### 4.3 Connection Pool Events

```
[debug] [db] pool.acquire idle_remaining=3 wait_us=12
[debug] [db] pool.release idle_remaining=4
```

`wait_us` measures the wall-clock time from `acquire()` entry to lock acquisition + queue availability. `idle_remaining` is the queue size after the operation completes.

### 4.4 Service Migration Estimate

Mechanical `tx.exec*` → `db::Exec(tx, sql, label, ...)` replacements per service file:

| File | Replacements |
|---|---|
| `vehicle_service.cc` | ~6 |
| `weather_service.cc` | ~5 |
| `source_category_service.cc` | ~6 |
| `consumption_service.cc` | ~6 |
| `charging_service.cc` | ~7 |
| `display_service.cc` | ~8 |
| **Total** | **~38** |

---

## 5. Configuration File Locations

### 5.1 `config.example.json` (repo root)

Committed to the repo as a dev/ops reference. Contents:

```json
{
  "database": {
    "url": "postgresql://evgrpc:dev@localhost:5432/evgrpc"
  },
  "oauth": {
    "issuer_url": "https://auth.example.com/",
    "audience": "evgrpc-api",
    "jwks_cache_ttl_seconds": 3600
  },
  "grpc": {
    "port": 50051
  },
  "log": {
    "level": "info",
    "file": "",
    "max_size_mb": 100,
    "max_files": 7
  }
}
```

### 5.2 Production Deployment

- `Dockerfile` `CMD`: `["evgrpc", "--config", "/etc/evgrpc/config.json"]`.
- Operators mount `config.json` into the container at `/etc/evgrpc/config.json` (via Docker volume, Kubernetes ConfigMap, or similar).
- Configuration changes require a container restart.

---

## 6. Testing Strategy

### 6.1 New Unit Tests

| File | Tests | Coverage |
|---|---|---|
| `tests/unit/test_config_loader.cc` | ~20 | per-field type / required / range / enum / URL-prefix validation; missing field error; unknown key error; default values; unreadable file; malformed JSON; multiple errors collected and reported together |
| `tests/unit/test_oidc_discovery.cc` | ~7 | happy path (local httplib::Server returns mock OIDC doc); 404; 500; connect/read timeout; missing `jwks_uri` field; malformed JSON; issuer URL with and without trailing `/` |
| `tests/unit/test_db_exec.cc` | ~6 | success path emits 2 debug lines with correct content; failure path emits 1 warn line with errcode + errmsg; `elapsed_us` is positive; parameter formatting for each supported type; 0-arg path works |
| `tests/unit/test_args.cc` | ~5 | `--config <path>`; `-c <path>`; `--help`; unknown flag; missing value after `--config` |
| **Total new** | **~38** | |

### 6.2 Updated Tests

| File | Change |
|---|---|
| `tests/unit/test_log.cc` | 6 places: `setenv("LOG_LEVEL", ...)` / `setenv("LOG_FILE", ...)` → build `LogConfig` literal and call `Init(cfg)`. The 2 test cases that exercise `LOG_FORMAT=json` warning behavior are deleted (feature removed). |
| `scripts/smoke.sh` | All `export DATABASE_URL=...` / `export OAUTH_*=...` / `export LOG_*=...` removed. Replaced with: `cp config.example.json /tmp/evgrpc-smoke.json` + run `evgrpc --config /tmp/evgrpc-smoke.json`. |
| `tests/integration/e2e_test.cc` | TestServer fixture passes `--config <test_config_path>` to the child process. |

### 6.3 Existing Regression

The 5 `tests/unit/test_*_service.cc` files (vehicle / weather / source_category / consumption / charging) are unchanged — they exercise business behavior, and the SQL calls are internal implementation details. The internal switch from `tx.exec*` to `db::Exec` should be transparent to service-level tests.

`tests/unit/test_display_service.cc` (Task 19) is similarly unchanged.

### 6.4 Test Count Target

- Before this spec: 38 tests passing (per `evgrpc-progress.json` 2026-07-31).
- After this spec: 38 (unchanged, no test deletions) + 38 (new) − 2 (deleted `LOG_FORMAT=json` tests) = **~74 tests passing**.

---

## 7. Build System Changes

### 7.1 `cmake/deps.cmake`

Promote `find_package(nlohmann_json 3.11.0 REQUIRED)` to the top-level (it currently lives inside the testcontainers-cpp guard). This makes the dep available to `src/` libraries, not just `tests/`.

No new `FetchContent` calls. No new top-level deps. `cpp_httplib` is already in `cmake/deps.cmake` and used by `src/auth/jwks_cache.cc`.

### 7.2 New `CMakeLists.txt` Files

- `src/config/CMakeLists.txt` — add `config_loader.cc` to the existing `evgrpc_lib` target
- `src/auth/CMakeLists.txt` — add `oidc_discovery.cc` to the existing `evgrpc_lib` target
- `src/db/CMakeLists.txt` — add `exec.cc` to the existing `evgrpc_lib` target
- `src/util/CMakeLists.txt` — add `args.cc` to the existing `evgrpc_lib` target
- `tests/unit/CMakeLists.txt` — add 4 new test files to the gtest target

---

## 8. Migration Sequence

1. `cmake/deps.cmake` — promote `find_package(nlohmann_json)`.
2. `src/config/config_loader.{h,cc}` — new file.
3. `src/config/config.{h,cc}` — replace env-var `Load()` with `SchemaConfig` / `RuntimeConfig` / `LoadConfig()`.
4. `src/util/args.{h,cc}` — new file.
5. `src/auth/oidc_discovery.{h,cc}` — new file.
6. `src/log/log.{h,cc}` — `Init()` → `Init(const LogConfig&)`.
7. `src/db/pool.cc` — add pool event logging.
8. `src/db/exec.{h,cc}` — new file.
9. `src/services/*_service.cc` (5) — replace `tx.exec*` with `db::Exec`.
10. `src/services/display_service.cc` — 8 replacements.
11. `src/main.cc` — argv parsing + full wiring.
12. `config.example.json` — new file at repo root.
13. `tests/unit/test_config_loader.cc` — new.
14. `tests/unit/test_oidc_discovery.cc` — new.
15. `tests/unit/test_db_exec.cc` — new.
16. `tests/unit/test_args.cc` — new.
17. `tests/unit/test_log.cc` — 6 setenv → LogConfig; delete 2 `LOG_FORMAT=json` tests.
18. `scripts/smoke.sh` — env exports → `--config` flag.
19. `Dockerfile` — `CMD ["evgrpc", "--config", "/etc/evgrpc/config.json"]`.
20. `README.md` — replace env-var section with config.json section.
21. `docs/superpowers/specs/2026-07-30-evgrpc-design.md` §9.3 — note "superseded by 2026-08-06-config-json-migration".

---

## 9. Documentation Updates

### 9.1 `README.md`

Replace the "Environment Variables" section with a "Configuration" section that:

- Shows `config.example.json` verbatim.
- Documents the `--config` / `-c` / `--help` flags.
- Explains the OIDC discovery behavior (no need to configure `jwks_url`).
- Lists all field constraints in a table (copy of §2.3).

### 9.2 `docs/superpowers/specs/2026-07-30-evgrpc-design.md`

§9.3 (Runtime Config) gains a one-line pointer: "Superseded by [`2026-08-06-config-json-migration.md`](./2026-08-06-config-json-migration.md)."

§5.4 (Configuration) gains: "JWKS URL is now auto-derived from the issuer URL via OIDC discovery — see `2026-08-06-config-json-migration.md` §3.1."

---

## 10. Open Questions

None at draft time. All design decisions were resolved during brainstorming:

- JSON library: `nlohmann_json` (already in build via testcontainers-cpp).
- OIDC discovery timing: eager at startup, cached for process lifetime.
- SQL Debug scope: full (statement + params + rows + elapsed + pool events + failure warn).
- Config file discovery: `./config.json` default + `--config` override.
- JSON structure: nested groups (`database` / `oauth` / `grpc` / `log`).
- Validation strictness: required + type + range + enum + URL-prefix + writable-parent-dir.

---

## 11. Out of Scope (Explicit)

Same as §1 "Out of Scope".

---

## 12. Revision History

| Date | Author | Change |
|---|---|---|
| 2026-08-06 | (brainstorming session) | Initial draft |
