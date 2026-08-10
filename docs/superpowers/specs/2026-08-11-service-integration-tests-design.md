# Service-Layer Integration Test Suite (with ≥95% Coverage)

- **Date:** 2026-08-11
- **Status:** Design (awaiting review)
- **Replaces:** none
- **Supersedes:** none

## 1. Background

`evGRpc` ships 6 gRPC services (27 RPCs, ~1758 LOC of service-implementation
code) with **zero service-level tests today**. Coverage tooling is not
installed. The only test that exercises a service is
`tests/integration/smoke_e2e_test.cc` — a single happy-path round-trip for
`VehicleService.CreateVehicle` / `ListVehicles`. Every other RPC, every
business-error branch, and every edge of `PgPool` / `db::Exec` is untested.

We want to ship a real test suite for the service layer, with coverage
enforcement, so that future RPCs don't land unverified and existing ones
have a safety net for the next refactor.

## 2. Goals

1. **Each of the 27 RPCs** has at least one integration test exercising the
   happy path through the real gRPC stack.
2. **Business-error branches** (NotFound, AlreadyExists, InvalidArgument,
   FailedPrecondition, …) reachable through the public API are covered.
3. **`src/services/*.cc` ≥ 95% line coverage** when the new test binary is
   run on its own. (a1's auth-failure branch is excluded; see §6.)
4. **Critical integration points** beyond the service layer are also
   covered: `PgContainer` real-path, `db::Exec` log lines, `db::Pool`
   acquire/release. (config-loader env-var fallback is already covered
   by the existing `test_config_loader.cc`; we re-confirm via a single
   assertion in `service_integration_main.cc` that `PgContainer`'s
   `SetUp()` succeeds with `DATABASE_URL` set.)
5. **Total runtime ≤ 60s** on the dev VM for the whole new binary.
6. The suite slots into the existing `ctest` workflow with **one extra
   `coverage` target** that produces an HTML report.

## 3. Non-Goals

- Branch/decision coverage enforcement (line coverage is enough for v1).
- Mock-DB unit tests for service internals. Real PG only.
- Performance benchmarks, fuzz tests, or property-based tests.
- A CI workflow change. The new target is runnable manually and via ctest
  locally; wiring it into a CI pipeline is a follow-up.
- Re-enabling testcontainers-cpp for ephemeral DBs. We stay on the shared
  local PG that the existing `PgContainer` already uses.
- Coverage of `auth/*` failure paths through the gRPC layer (the existing
  `test_authenticate.cc` covers the validator; see §6).

## 4. Architecture

```
ctest (evgrpc_integration_tests)
  └─ Global env (one process):
       ├─ PgContainer singleton (real PG @ 127.0.0.1:5432/evgrpc)
       ├─ TestServer singleton (in-process gRPC + JWKS HTTP + bearer creds)
       │     - jwt_validator_.bypass = true   (a1)
       └─ Truncate helper: TRUNCATE vehicle, charging, consumption,
            source_category, weather CASCADE  (per-test)
  └─ Per-test: shared gRPC channel, generated stub, RPC call, assert.
```

The suite uses **one process, one PG, one in-process gRPC server** —
launched once via a `::testing::Environment`. Each `TEST_F` issues a
`TruncateAll()` (a new helper) before any stateful work, then runs a
single RPC. This keeps the runtime under 60s for ~75 cases. **Trade-off
note (vs. the existing `pg_container.h` rationale):** the original
`PgContainer` did not auto-truncate because the v1 smoke test only
touched one row. With ~75 cases across 7 tables and parallel-style
iteration, an explicit per-test `TRUNCATE` at the call site becomes
unbearable copy-paste; centralizing in `SharedPgEnvironment::TruncateAll()`
called from a `ServiceITBase` `SetUp()` keeps it explicit (the call is
right there in every fixture) while removing the duplication.

### 4.1 Fixture mode: `no_auth`

`TestServer` is extended with a `TestServer::Options` struct:

```cpp
struct Options {
  bool no_auth = false;   // skip JWT validation; do not start JWKS HTTP server
  std::shared_ptr<PgContainer> pg;
};
class TestServer {
 public:
  explicit TestServer(Options opts);
  // ... existing public API unchanged
};
```

`no_auth=true` does two things together: (1) constructs a `JwtValidator`
with `bypass=true` (see §5.2), and (2) skips the JWKS HTTP server
entirely. The two flags are intentionally fused — there is no use case
in this codebase for "JWT validation on but JWKS server off" or vice
versa. When `no_auth=true` is set, RPCs succeed regardless of the bearer
token (or absence thereof); tests do **not** need to mint tokens.

When `no_auth=false`, behavior is identical to today (real JWT
validation against the embedded JWKS server). The smoke test stays on
this path.

### 4.2 Shared PG

`PgContainer` already supports a shared instance. We add a small
singleton helper next to the fixture (e.g. `tests/fixtures/shared_pg.h`):

```cpp
class SharedPgEnvironment : public ::testing::Environment {
 public:
  void SetUp() override;   // creates PgContainer + applies sql/*.sql
  void TearDown() override;
  static std::shared_ptr<PgContainer> pg();
  static void TruncateAll();  // TRUNCATE ... CASCADE; idempotent
};
```

Registered with `AddGlobalTestEnvironment` from `main()` of the new
binary. **No `TRUNCATE` in the `PgContainer` constructor** — explicit at
the test site for visibility.

### 4.3 Per-RPC test shape

```cpp
TEST_F(VehicleServiceIT, CreateVehicle_HappyPath) {
  TruncateAll();
  auto stub = VehicleService::NewStub(channel());
  Vehicle v = MakeValidVehicle();   // local helper
  Vehicle resp;
  grpc::ClientContext ctx;
  // No bearer credentials attached: no_auth=true bypasses.
  grpc::Status st = stub->CreateVehicle(&ctx, v, &resp);
  EXPECT_TRUE(st.ok()) << st.error_message();
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.brand(), v.brand());
}

TEST_F(VehicleServiceIT, GetVehicle_NotFound) {
  TruncateAll();
  auto stub = VehicleService::NewStub(channel());
  GetVehicleRequest req;
  req.set_id("00000000-0000-0000-0000-000000000000");
  Vehicle resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicle(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(VehicleServiceIT, CreateVehicle_DuplicateLicensePlate_Conflict) {
  TruncateAll();
  auto stub = ...;
  Vehicle v1 = MakeValidVehicle("TESLA-1");
  Vehicle r1; stub->CreateVehicle(&ctx, v1, &r1);  // OK
  Vehicle v2 = MakeValidVehicle("TESLA-1");        // same plate
  Vehicle r2; grpc::Status st = stub->CreateVehicle(&ctx, v2, &r2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}
```

Each `TEST_F` is self-contained: a `ServiceITBase : public ::testing::Test`
fixture provides `channel()` and a `SetUp()` that calls
`SharedPgEnvironment::TruncateAll()`. Per-service test fixtures (e.g.
`VehicleServiceIT : public ServiceITBase`) inherit this; the call to
`TruncateAll()` is therefore **at the test site, not hidden in
`SharedPgEnvironment`'s `SetUp()`** — it runs once per `TEST_F` and is
visible in the test base's `SetUp()`.

## 5. Implementation Plan (overview only — see implementation plan for tasks)

### 5.1 New files

```
tests/integration/
  service_integration_main.cc       (gtest_main wrapper, registers env)
  service_test_fixtures.h/.cc       (ServiceITBase, TruncateAll, helpers)
  vehicle_service_test.cc           (5 RPCs × 2-3 cases = 10-15)
  charging_service_test.cc          (5 × 2-3 = 10-15)
  consumption_service_test.cc       (5 × 2-3 = 10-15)
  display_service_test.cc           (8 × 2-3 = 16-24)
  source_category_service_test.cc   (2 × 2-3 = 4-6)
  weather_service_test.cc           (2 × 2-3 = 4-6)
tests/fixtures/
  shared_pg.h/.cc                   (singleton env + TruncateAll)
docs/superpowers/
  specs/2026-08-11-service-integration-tests-design.md    (this file)
  plans/2026-08-11-service-integration-tests.md          (next step)
```

### 5.2 Source changes

**`src/auth/jwt_validator.{h,cc}`** — add a non-functional bypass field:

```cpp
struct JwtValidator {
  std::string issuer;
  std::string audience;
  std::function<std::optional<std::string>(const std::string& kid)> resolve_key;
  bool bypass = false;   // NEW: when true, Validate always returns success.
  std::optional<Claims> Validate(const std::string& token) const;
};
```

In `Validate()`:

```cpp
if (bypass) {
  return Claims{ /* subject */ "test-subject",
                 /* issuer  */ issuer,
                 /* audience*/ audience };
}
// existing logic unchanged
```

This is a 4-line additive change. **No public-API break** (default is
`false`).

**`tests/fixtures/test_server.{h,cc}`** — add the `Options` ctor:

```cpp
struct TestServer::Options {
  bool no_auth = false;
  std::shared_ptr<PgContainer> pg;
};
explicit TestServer(TestServer::Options opts);
// existing ctor `explicit TestServer(std::shared_ptr<PgContainer>)`
// delegates with Options{ .pg = std::move(pg) } for back-compat.
```

When `no_auth=true`:
- Construct `JwtValidator{ .issuer = iss_, .audience = aud_, .bypass = true }`.
- **Skip** the JWKS HTTP server (the two are fused — see §4.1).
- The `BearerTokenCredentials()` method is unchanged on the wire: it
  still signs a real RS256 JWT on every call. Server-side bypass simply
  ignores the token. (A future optimization could short-circuit client
  signing in `no_auth` mode to save ~ms/case, but is **not** in v1 — the
  signing cost is well under the 0.8 s/case budget.)

**`tests/fixtures/test_server.h`** — keep the existing `TestServer(PgContainer)` ctor as a thin delegator. Don't break the smoke test.

### 5.3 CMake / build

**`tests/integration/CMakeLists.txt`** — append a new executable:

```cmake
add_executable(evgrpc_integration_tests
  service_integration_main.cc
  service_test_fixtures.cc
  vehicle_service_test.cc
  charging_service_test.cc
  consumption_service_test.cc
  display_service_test.cc
  source_category_service_test.cc
  weather_service_test.cc
)
target_link_libraries(evgrpc_integration_tests PRIVATE
  gtest
  gtest_main
  evgrpc_test_fixtures
  evgrpc_proto
  evgrpc_db          # db::Exec / PgPool
)
add_test(NAME evgrpc_integration_tests COMMAND evgrpc_integration_tests)
```

**`CMakeLists.txt` (top-level)** — add a coverage preset:

```cmake
option(EVGRPC_COVERAGE "Enable coverage instrumentation (gcov)" OFF)
if(EVGRPC_COVERAGE)
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()
```

Reason for `-O0`: coverage must be built without optimization or line
attribution drifts. The CMake build type stays whatever the dev sets
(`Debug` for normal work, the new options kick in for coverage runs).

### 5.4 Coverage invocation

A shell script `scripts/coverage.sh` (new, executable, ~30 lines):

- **Inputs:** none (reads from repo root).
- **Outputs:** `cmake-build-cov/coverage.info`, `cmake-build-cov/coverage_html/index.html`, one-line `lcov` summary on stdout.
- **Exit codes:** 0 = success (≥95% on `src/services/`); 1 = below threshold OR ctest OR lcov failed.

```bash
cmake -S . -B cmake-build-cov -DEVGRPC_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-cov -j
ctest --test-dir cmake-build-cov -R evgrpc_integration_tests --output-on-failure
lcov --capture --directory cmake-build-cov \
     --output-file cmake-build-cov/coverage.info \
     --ignore-errors mismatch \
     --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
genhtml cmake-build-cov/coverage.info \
        --output-directory cmake-build-cov/coverage_html
```

The script prints a one-line summary of `lines......: XX.X% (NNN/NNN)`
parsed from `coverage.info`; non-95% on `src/services/*` fails the
script (exit 1). Excludes are hard-coded and documented in the script
header.

## 6. Coverage Scope and the 95% Gate

| Included | Excluded |
|---|---|
| `src/services/*.cc` (target ≥ 95%) | `src/proto/*.proto` |
| `src/db/exec.cc`, `src/db/pool.cc` (critical integration) | `cmake-build-*/generated/*` (proto codegen) |
| `src/auth/authenticate_rpc.h` (used by every service) | `_deps/*` (third-party) |
| `tests/fixtures/pg_container.cc` (real-path) | `tests/*` (test code) |
| `src/config/config_loader.cc` env-var path (already tested; just need to confirm) | `src/main.cc` (entry point, no logic) |
| | `src/auth/jwt_validator.cc`, `src/auth/jwks_cache.cc`, `src/auth/oidc_discovery.cc` (a1 excludes; existing unit tests cover them) |

The a1 trade-off: `if (!a.status.ok())` inside every RPC method (the
auth-failure early return) is **not** covered by this suite. To still
hit ≥95% on `src/services/*.cc`, every test case must exercise both
**the happy path and a business-error branch** of its target RPC. Concretely:

- Each `Create*` RPC: 1 happy + 1 duplicate-id (ALREADY_EXISTS) + 1
  invalid-argument.
- Each `Get*` RPC: 1 happy + 1 not-found.
- Each `Update*` RPC: 1 happy + 1 not-found + 1 invalid-argument.
- Each `Delete*` RPC: 1 happy + 1 not-found.
- Each `List*` RPC: 1 happy + 1 empty (no rows) + 1 filtered.
- Each `Search*` RPC (`SearchWeather`, `SearchSourceCategory` — 2 RPCs
  total): 1 happy + 1 empty (zero matches) + 1 filtered (no results for
  filter). Follows the `List*` pattern.
- `DisplayService` aggregation RPCs (`GetVehicleCostSummary`,
  `GetMonthlyReport`, `GetAnnualReport`): 1 happy + 1 empty-result,
  which on these three RPCs returns `grpc::StatusCode::INTERNAL` with
  message `"no aggregate row"` (verified: `src/services/display_service.cc`
  lines 82, 160, 224). The 5 remaining `DisplayService` RPCs follow
  the `Get*`/`List*` patterns.

That brings the **target case count to ~60-75**. Realistic per-service
breakdown is in §5.1.

Auth-failure coverage through gRPC is added later as a separate
`auth_failure_e2e_test.cc` (~4-6 cases) that uses `no_auth=false` and
exercises missing / expired / wrong-issuer tokens. This brings auth
coverage of the service entry point back in. **Out of scope for v1**;
called out in §3.

## 7. Test Data Helpers

A small `TestData` namespace in `service_test_fixtures.h`:

```cpp
namespace evgrpc::test::data {
  Vehicle MakeValidVehicle(std::string plate = "TEST-" + uuid);
  Charging MakeValidCharging(std::string vehicle_id);
  Consumption MakeValidConsumption(std::string vehicle_id);
  // ... etc.
  std::string FreshUuid();
  std::string FreshTimestampIso();
}
```

Helpers generate fully-valid messages with unique IDs so case-to-case
isolation is enforced even without `TRUNCATE`. Truncation is the
backstop, not the primary isolation mechanism.

## 8. Error-Branch Strategy

| Error path | How we hit it |
|---|---|
| `NOT_FOUND` | Request a UUID that was never inserted |
| `ALREADY_EXISTS` | Insert once, insert again with same unique field (license plate, source name, etc.) |
| `INVALID_ARGUMENT` | Send empty required field, malformed UUID, etc. — depends on per-RPC validation |
| `INTERNAL` ("no aggregate row") | Call `GetVehicleCostSummary` / `GetMonthlyReport` / `GetAnnualReport` against an empty database — deterministic, no mocking needed |
| `FAILED_PRECONDITION` | Not exercised in v1 — `src/services/*.cc` does not return it from any current branch. Future RPCs may add this. |

## 9. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Real PG schema drift between dev DB and `sql/001_initial.sql` | `TruncateAll()` runs `BEGIN;` and applies the SQL on first startup. Re-apply on mismatch in shared env SetUp. |
| Test runtime creeps over 60s | `cases_per_second` budget enforced in `scripts/coverage.sh` — fail if total wall-clock > 90s. |
| `VehicleServiceIT` parallel runs collide on shared PG | Single-threaded by default (gtest's default); `SharedPgEnvironment` is documented as not thread-safe. |
| `bypass=true` accidentally shipped | `JwtValidator::bypass` is unit-tested in `test_jwt_validator.cc` for `false`-default; CI build asserts `grep -RIn 'bypass = true' src/` is empty. |
| Generated proto stubs change and break stub API | New binary is added to the existing `evgrpc_proto` consumer list; same regenerate-on-proto-change rule. |
| `DisplayService` 8 RPCs are the bulk of the LOC and the slowest to spec | Implementation plan tasks are ordered: simpler CRUD services first, `DisplayService` last with its own design checkpoint. |

## 10. Acceptance Criteria

The implementation plan is **done** when:

1. `cmake --build` succeeds with `-DEVGRPC_COVERAGE=ON`.
2. `ctest -R evgrpc_integration_tests` passes 100%, ≤ 60s wall-clock.
3. `lcov` summary reports `lines......: 95.0%` or higher on
   `src/services/`.
4. The smoke test (`evgrpc_e2e_tests`) still passes unchanged.
5. `grep -RIn 'bypass = true' src/ tests/` returns **exactly one
   match**, in `tests/fixtures/test_server.cc`'s `TestServer::Impl` ctor.
   Zero matches would mean the bypass is dead code; ≥2 would mean
   production code accidentally toggles auth off.
6. A `scripts/coverage.sh` runs end-to-end and exits 0.

## 11. Out-of-Scope Follow-ups (next specs)

- `auth_failure_e2e_test.cc` — auth-failure branches through gRPC.
- Property-based tests for `DisplayService` analytics.
- Coverage of `src/util/req_id.cc`, `src/util/uuid.cc` — small, low
  value, separate effort.
- CI integration: GitHub Actions matrix, coverage badge, diff coverage.
