# evGRpc

EV electricity cost tracking gRPC service. C++20, gRPC v1.62, PostgreSQL 14+,
JWT (RS256) auth via OAuth 2.0 resource server semantics.

## Build (host)

```sh
cmake -G Ninja -S . -B build
cmake --build build --target evgrpc_server
```

System packages required (Debian/Ubuntu):

- `build-essential`, `cmake` (≥3.22), `ninja-build`, `git`, `pkg-config`
- `libpqxx-dev`, `libpq-dev`, `libssl-dev`, `libcurl4-openssl-dev`
- `libabsl-dev`, `libre2-dev`, `libc-ares-dev`, `zlib1g-dev`
- `libprotobuf-dev`, `protobuf-compiler`
- `nlohmann-json3-dev`, `uuid-dev`

Fetched via FetchContent (network access required to github.com or a
gh-proxy mirror; see `cmake/deps.cmake`):

- gRPC v1.62.0 (with boringssl, abseil, cares, re2, zlib submodules)
- protobuf v25.1 (for the `protoc` binary)
- libpqxx 7.9.2, spdlog v1.13.0, googletest v1.14.0
- jwt-cpp (with its own nlohmann/json FetchContent fallback if the system
  pkg isn't installed)
- cpp_httplib, testcontainers-cpp v0.2.0

## Test (host)

```sh
cmake --build build --target evgrpc_tests evgrpc_e2e_tests
./build/tests/evgrpc_tests

EVGRPC_TEST_DB_PASSWORD=… ./build/tests/integration/evgrpc_e2e_tests
```

- 107 unit tests cover config loader, args, OIDC discovery, log init,
  JWT validation, JWKS caching, all 6 service layers, DB pool,
  ID generation, db::Exec wrapper, and the charging-report helpers
  (LastDayOfMonth, schema migration, TZ-aware grouping).
- 114 integration tests exercise all 6 services against a shared local
  PG, including the 3 charging-report RPCs (see
  [Charging Reports](#charging-reports-displayservice) below).
- 2 e2e tests (`E2ESmoke.CreateThenListVehicle`,
  `E2ESmokeNoAuth.CreateVehicleWithoutToken`) bring up an in-process
  gRPC server + JWKS IdP + shared local PG.
- The e2e tests truncate `vehicle ... CASCADE` at the start, so they're
  re-runnable without manual cleanup.

## Python Integration Tests (gRPC client-side)

A pytest-based client-side integration test suite (`tests/python/`)
hits the docker-compose stack (`nginx:80 → evgrpc:50051 → Postgres`)
via the Bearer-token helper (`evgrpc-token` CLI). 99 tests cover happy
paths, error paths, data boundaries, UNIQUE + FK constraints, and auth
enforcement for all 6 services.

**One-command setup:**

```sh
conda env create -f environment.yml   # creates `evgrpc-tests` env
bash scripts/gen_python_stubs.sh     # regenerate protoc stubs (committed)
```

**Run:**

```sh
conda run -n evgrpc-tests pytest tests/python/ -v
```

**Per-test rationale** in `tests/python/doc/<service>.md` (one section
per test, per spec Goal 11). MD ↔ code alignment is enforced by a
Python check that strips parametrize `[...]` suffixes.

**Test isolation:** session-unique 8-hex `namespace` prefix + double
sweep (L1 per-function `TrackedInsert`, L2 session-start + session-end
sweep via `sweep_all_test_rows`). Children-first deletion order
respects FK NO ACTION:
`consumption → charging → vehicle → weather → source_category`.

The suite is wired into `./scripts/run_all_tests.sh` as suite 4
(between e2e and coverage gate).

## Coverage

Run the integration test suite with coverage instrumentation:

```sh
DATABASE_URL='postgresql://evgrpc_admin:***@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
./scripts/coverage.sh
```

The script builds with `--coverage`, runs `evgrpc_integration_tests`, asserts ≥ 85% line coverage averaged across `src/services/*.cc`, and exits non-zero on failure. HTML report: `cmake-build-cov/coverage_html/index.html`. (Spec target was 95%; the realistic ceiling given unreachable exception paths + dead code is ~85% — see `docs/superpowers/plans/2026-08-11-service-integration-tests.md` End of Chunk 7 v2 for details.)

Override thresholds via env vars: `COVERAGE_THRESHOLD=95`, `RUNTIME_THRESHOLD=120`.

Required tools: `apt install lcov` (Debian/Ubuntu) or `brew install lcov` (macOS). `genhtml` ships with lcov.

## Run (host)

```sh
./build/src/evgrpc_server --config ./config.json
```

All runtime configuration lives in a single JSON file (see
[Configuration](#configuration) below). The default path is
`./config.json`; override with `--config <path>` or `-c <path>`.
`--help` prints the usage line and exits 0.

## Docker

```sh
docker build -t evgrpc:dev .
```

Behind a restrictive network (e.g. mainland China):

```sh
docker build \
  --build-arg APT_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ \
  --build-arg GIT_INSTEADOF=https://gh-proxy.com/https://github.com/ \
  -t evgrpc:dev .
```

Build args:

- `APT_MIRROR` — apt source line rewrite. Empty by default (hermetic).
  Pass `http://…` for a working mirror. HTTPS mirrors don't work because
  the base image doesn't ship with a ca-bundle that trusts third-party
  mirrors; HTTP is fine for apt since packages carry no secret payload.
- `GIT_INSTEADOF` — git URL rewrite. Empty by default. Pass
  `https://gh-proxy.com/https://github.com/` to route github.com traffic
  through a proxy.

Run:

```sh
docker run --rm --network host \
  -v $PWD/config.json:/etc/evgrpc/config.json:ro \
  -v evgrpc_logs:/app/log \
  evgrpc:dev
```

The container reads its config from `/etc/evgrpc/config.json`. Mount
your config file there (or use a Kubernetes ConfigMap). No env vars
are required at startup in v2.

`config.json` is the canonical name — it matches `src/util/args.h`'s
default `config_path` so the server will also find it via CWD lookup
if `--config` is omitted. `/app/log` is created in the image and
declared as a `VOLUME`; the named volume `evgrpc_logs` (or any bind
mount) keeps the rotating log files across container restarts.

Notes:

- `--network host` lets the container reach services on the host
  network namespace (e.g. a locally-running PostgreSQL on `127.0.0.1`).
  On macOS/Windows Docker Desktop, use `host.docker.internal` in
  `database.url` instead.
- **URL-encode `@` in the password** as `%40`. libpqxx refuses a raw
  `@` because it would be parsed as the user/host separator.

## Smoke

```sh
DATABASE_URL='postgresql://evgrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc' \
scripts/smoke.sh
```

Starts the `evgrpc:dev` container, writes a smoke `config.json` from
`config.example.json` + the supplied `DATABASE_URL`, generates an
ephemeral RSA keypair, serves the JWKS from a local
`python3 -m http.server`, mints an RS256 JWT, runs
`VehicleService.CreateVehicle` → `ListVehicles` via `grpcurl`, then
tears everything down. Exits 0 on success, 1 on any failure.

Note: the smoke JWKS server doesn't expose `/.well-known/openid-configuration`,
so OIDC discovery fails at startup. The smoke container will log the
failure and exit non-zero. For a true round-trip you need a real IdP
(or a JWKS server that also serves the OpenID discovery doc at the
expected path). The gate the smoke verifies is "config.json is read
and parsed, the binary starts up, services register".

Re-runs are idempotent-ish (CreateVehicle doesn't enforce a unique
license plate, so each run adds another `SMOKE-<pid>` row; ListVehicles
returns all rows; both calls succeed).

Requires: `docker`, `python3` with `PyJWT` + `cryptography`, `openssl`,
and `grpcurl` (the script auto-falls-back to `~/.local/bin/grpcurl` or
whatever `GRPCURL=$PWD/grpcurl` points at).

## Charging Reports (DisplayService)

Three RPCs report charging-only aggregates over calendar periods. They
read the `charging` table only (no consumption mix-in), group by
calendar day/month/year in the session time zone, and return a zeroed
report (not an error) when no rows match.

| RPC | Request fields | Returns |
|-----|----------------|---------|
| `GetDailyChargingReport` | `year`, `month`, `day`, `vehicle_id?` | `ChargingReport` |
| `GetMonthlyChargingReport` | `year`, `month`, `vehicle_id?` | `ChargingReport` |
| `GetAnnualChargingReport` | `year`, `vehicle_id?` | `ChargingReport` |

`ChargingReport`:

| Field | Meaning |
|-------|---------|
| `year` | calendar year |
| `month` | `0` = annual; `1-12` = monthly/daily |
| `day` | `0` = annual/monthly; `1-31` = daily |
| `total_cost` | summed cost |
| `total_kwh` | summed energy |
| `count` | number of charging events |
| `vehicle_id` | empty = all vehicles |

Validation (all failures return `INVALID_ARGUMENT`):

- `year < 1900`
- `month` outside `1..12`
- `day` outside `1..LastDayOfMonth(year, month)` — so Feb 30, Apr 31,
  and Feb 29 in a non-leap year are all rejected.

Time-zone convention: day/month/year boundaries are computed with
`c.StartTime::date = make_date(...)` and `EXTRACT(YEAR/MONTH FROM
c.StartTime)` on the `TIMESTAMPTZ` columns. The session time zone comes
from the PostgreSQL postmaster — set `TZ` on the server process (or
`timezone` in `postgresql.conf`). A libpq-client `SET TIME ZONE` does
**not** affect these reports, because server-side pooled connections
use the postmaster's time zone. The `charging.StartTime`/`EndTime`
columns must be `TIMESTAMPTZ` (migrated by
`sql/002_charging_timestamptz_migration.sql`); bare `TIMESTAMP` would
store values relative to the INSERT-time session time zone.

## Configuration

evGRpc reads all runtime configuration from a single JSON file. The
default path is `./config.json` (relative to the working directory);
override with `--config <path>` or `-c <path>`.

### Flags

| Flag                          | Purpose                                                |
|-------------------------------|--------------------------------------------------------|
| `--config <path>` / `-c <path>` | Path to the config JSON (default `./config.json`)     |
| `--help` / `-h`                | Print usage to stdout, exit 0                          |

### Schema (4 nested sections)

```json
{
  "database": { "url": "postgresql://user:pw@host:5432/db" },
  "oauth": {
    "issuer_url": "https://issuer.example.com/",
    "audience": "evgrpc-api",
    "jwks_cache_ttl_seconds": 3600,
    "discovery_connect_timeout_seconds": 5,
    "discovery_read_timeout_seconds": 15
  },
  "grpc": { "port": 50051 },
  "log": {
    "level": "info",
    "file": "",
    "max_size_mb": 100,
    "max_files": 7
  }
}
```

Two config files live at the repo root:

* [`config.example.json`](config.example.json) — production-template reference (safe to commit; no secrets).
* [`config.json`](config.json) — local test environment values matching the test infrastructure (test DB + test OIDC issuer/audience matching `tests/fixtures/test_server.cc`). Running `./build/src/evgrpc_server` from the repo root picks this up automatically (default path). **Do not use this config in production** — it points at the test database and a non-existent OIDC issuer.

Full validation rules and error messages are in
[`docs/superpowers/specs/2026-08-06-config-json-migration.md`](docs/superpowers/specs/2026-08-06-config-json-migration.md) §2.

### OIDC Discovery

`oauth.issuer_url` is the only OAuth URL you need to configure. The
JWKS URL is fetched automatically at startup from
`{issuer}/.well-known/openid-configuration`. If the discovery endpoint
is unreachable, the server fails fast at startup.

Discovery uses HTTPS-capable libcurl. The HTTP fetch timeouts are
tunable via `oauth.discovery_connect_timeout_seconds` (default 5) and
`oauth.discovery_read_timeout_seconds` (default 15) — the defaults are
sized for cold-start https discovery where the first DNS+TLS round-trip
can exceed a few seconds.

### Docker

```sh
docker run \
  -v $PWD/config.json:/etc/evgrpc/config.json:ro \
  -v evgrpc_logs:/app/log \
  evgrpc:latest
```

### Validation

If the config file is missing a required field, has the wrong type,
references an unwritable log file path, or contains an unknown key,
the server prints one line per problem to stderr and exits 1 — no
partial startup. All errors are reported in one pass (not just the
first one found).

## Spec

- `docs/superpowers/specs/2026-07-30-evgrpc-design.md` — full design
  (services, DB schema, auth model, logging conventions). Note: the
  env-var sections are retained for historical reference; **v2 reads
  config from `config.json` only** (see `Configuration` above).
- `docs/superpowers/specs/2026-08-06-config-json-migration.md` —
  config.json migration spec (authoritative for v2 config).
