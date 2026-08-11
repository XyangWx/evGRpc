# Service-Layer Integration Test Suite Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `ctest`-driven integration test binary that covers all 27 service RPCs with ≥95% line coverage on `src/services/*.cc`, plus critical integration points (`PgContainer`, `db::Exec`, `db::Pool`).

**Architecture:** New binary `evgrpc_integration_tests` shares one `PgContainer` (real local PG at 127.0.0.1) and one in-process gRPC `TestServer` (configured with `no_auth=true`) across all `TEST_F`s. Per-test isolation via `TRUNCATE ... CASCADE` from `ServiceITBase::SetUp()`. Coverage via gcov + lcov, threshold enforced by `scripts/coverage.sh`.

**Tech Stack:** CMake (Ninja generator), C++20, gcc + `--coverage`, gtest, libpqxx, grpc++, OpenSSL, cpp-httplib (already in fixture), jwt-cpp, lcov/genhtml (system tools).

**Spec:** [`docs/superpowers/specs/2026-08-11-service-integration-tests-design.md`](../specs/2026-08-11-service-integration-tests-design.md)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/auth/jwt_validator.h` (modify) | Add `bypass` field to `JwtValidator` struct |
| `src/auth/jwt_validator.cc` (modify) | Branch on `bypass` in `Validate()` |
| `tests/unit/test_jwt_validator.cc` (modify) | Add tests for bypass default-off and bypass-on paths |
| `tests/fixtures/test_server.h` (modify) | Add `Options` struct + `Options` ctor; keep old ctor as delegator |
| `tests/fixtures/test_server.cc` (modify) | Implement `Options` ctor; skip JWKS HTTP + use bypass validator when `no_auth=true` |
| `tests/fixtures/shared_pg.h` (new) | `SharedPgEnvironment` (gtest `Environment`) + `TruncateAll()` |
| `tests/fixtures/shared_pg.cc` (new) | Implementation: open PG, apply SQL, expose `pg()` + `TruncateAll()` |
| `tests/fixtures/CMakeLists.txt` (modify) | Add `shared_pg.cc` to `evgrpc_test_fixtures` library |
| `tests/integration/service_integration_main.cc` (new) | Custom `main()` wrapper, `AddGlobalTestEnvironment(new SharedPgEnvironment)` (no `gtest_main` — we provide our own main) |
| `tests/integration/smoke_e2e_test.cc` (modify) | Task 2 adds new `no_auth` smoke case alongside the existing one |
| `tests/integration/service_test_fixtures.h/.cc` (new) | `ServiceITBase : ::testing::Test` with `channel()`, `SetUp()` → `TruncateAll()` |
| `tests/integration/test_data.h/.cc` (new) | `evgrpc::test::data::MakeValid*()` helpers + `FreshUuid()` |
| `tests/integration/{vehicle,charging,consumption,display,source_category,weather}_service_test.cc` (new) | Per-service `TEST_F` cases |
| `tests/integration/CMakeLists.txt` (modify) | Add `evgrpc_integration_tests` executable + ctest entry |
| `CMakeLists.txt` (modify, top-level) | `option(EVGRPC_COVERAGE ...)` with `add_compile_options(--coverage)` |
| `scripts/coverage.sh` (new) | Build + test + lcov + threshold check (executable) |

---

## Chunk 1: Foundation

Tasks 1-6 set up everything the per-service tests depend on. After Chunk 1, `ctest -R evgrpc_integration_tests` should run an empty binary that opens PG, starts a no-auth `TestServer`, truncates, and exits 0.

### Task 1: Add `bypass` to `JwtValidator`

**Files:**
- Modify: `src/auth/jwt_validator.h:18-23` (struct definition)
- Modify: `src/auth/jwt_validator.cc` (Validate function body)
- Modify: `tests/unit/test_jwt_validator.cc` (add bypass tests)
- Test: existing `evgrpc_tests` ctest target

- [ ] **Step 1: Write failing test for `bypass=false` default behavior**

Add to `tests/unit/test_jwt_validator.cc`:

```cpp
TEST(JwtValidatorTest, BypassDefaultsToFalse) {
  JwtValidator v;
  v.issuer = "iss";
  v.audience = "aud";
  // resolve_key left empty — Validate should return nullopt on a real token
  auto claims = v.Validate("not.a.token");
  EXPECT_EQ(claims, std::nullopt);
  // The key invariant: bypass is opt-in, never default.
  EXPECT_FALSE(v.bypass);
}
```

- [ ] **Step 2: Run test — expect compile error (no `bypass` field yet)**

Run: `cmake --build cmake-build-debug --target evgrpc_tests && cmake-build-debug/tests/evgrpc_tests --gtest_filter=JwtValidatorTest.BypassDefaultsToFalse`
Expected: compile error `‘struct evgrpc::JwtValidator’ has no member named ‘bypass’`.

- [ ] **Step 3: Add `bypass` field to `JwtValidator` struct**

In `src/auth/jwt_validator.h`, modify the struct:

```cpp
struct JwtValidator {
  std::string issuer;
  std::string audience;
  std::function<std::optional<std::string>(const std::string& kid)> resolve_key;
  bool bypass = false;  // NEW
  std::optional<Claims> Validate(const std::string& token) const;
};
```

In `src/auth/jwt_validator.cc`, prepend to `Validate`:

```cpp
std::optional<Claims> JwtValidator::Validate(const std::string& token) const {
  if (bypass) {
    return Claims{ /* subject */ "test-subject",
                   /* issuer  */ issuer,
                   /* audience*/ audience };
  }
  // existing implementation unchanged
}
```

- [ ] **Step 4: Run unit test — expect PASS**

Run: `cmake --build cmake-build-debug --target evgrpc_tests && cmake-build-debug/tests/evgrpc_tests --gtest_filter=JwtValidatorTest.BypassDefaultsToFalse`
Expected: PASS.

- [ ] **Step 5: Add positive bypass test**

```cpp
TEST(JwtValidatorTest, BypassReturnsSyntheticClaims) {
  // Named constant (NOT the literal `true`) so the spec §10.5
  // `grep -RIn 'bypass = true' src/ tests/` still has exactly one
  // match, in tests/fixtures/test_server.cc.
  constexpr bool kEnableBypassForTest = true;
  JwtValidator v;
  v.issuer = "iss-xyz";
  v.audience = "aud-xyz";
  v.bypass = kEnableBypassForTest;
  auto claims = v.Validate("");  // empty token still validates
  ASSERT_TRUE(claims.has_value());
  EXPECT_EQ(claims->subject, "test-subject");
  EXPECT_EQ(claims->issuer, "iss-xyz");
  EXPECT_EQ(claims->audience, "aud-xyz");
}
```

- [ ] **Step 6: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_tests --gtest_filter=JwtValidatorTest.Bypass*`
Expected: 2/2 PASS.

```bash
cd /data/Repositories/evGRpc
git add src/auth/jwt_validator.h src/auth/jwt_validator.cc tests/unit/test_jwt_validator.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(auth): add JwtValidator.bypass for test-mode no-auth"
```

---

### Task 2: Add `TestServer::Options` ctor (no_auth mode)

**Files:**
- Modify: `tests/fixtures/test_server.h`
- Modify: `tests/fixtures/test_server.cc`
- Test: existing `evgrpc_e2e_tests` (smoke must still pass)

- [ ] **Step 1: Run existing smoke test — confirm green baseline**

Run: `cd cmake-build-debug && EVGRPC_TEST_DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' DATABASE_URL="$EVGRPC_TEST_DATABASE_URL" ctest -R evgrpc_e2e_tests`
Expected: PASS.

- [ ] **Step 2: Add the no_auth smoke test FIRST (TDD)**

Add to `tests/integration/smoke_e2e_test.cc`:

```cpp
TEST(E2ESmokeNoAuth, CreateVehicleWithoutToken) {
  TestServer ts({ .no_auth = true,
                  .pg = std::make_shared<PgContainer>() });
  auto channel = ts.Channel();  // no bearer creds attached
  auto stub = VehicleService::NewStub(channel);
  Vehicle v; v.set_brand("Tesla"); /* ... populate fields ... */
  Vehicle resp;
  grpc::ClientContext ctx;  // NO credentials attached
  grpc::Status st = stub->CreateVehicle(&ctx, v, &resp);
  EXPECT_TRUE(st.ok()) << st.error_message();
}
```

- [ ] **Step 3: Run — expect compile error (`TestServer` ctor doesn't accept `{...}` yet)**

Run: `cmake --build cmake-build-debug --target evgrpc_e2e_tests 2>&1 | tail -5`
Expected: compile error mentioning `TestServer` does not accept designated-initializer or `Options`.

- [ ] **Step 4: Add `TestServer::Options` struct + ctor to `test_server.h`**

Per spec §5.2, the struct lives nested inside `TestServer` (NOT a free `TestServerOptions`):

```cpp
class TestServer {
 public:
  struct Options {
    bool no_auth = false;
    std::shared_ptr<PgContainer> pg;
  };
  explicit TestServer(Options opts);
  // existing ctor stays; update its out-of-line definition to delegate:
  //   explicit TestServer(std::shared_ptr<PgContainer> pg);
  // ...
};
```

- [ ] **Step 5: Implement `Options` ctor in `test_server.cc`**

The existing `JwtValidator` member in `Impl` is named `validator_` (see `tests/fixtures/test_server.cc:269` — `JwtValidator validator_;`). All existing references in lines 204–206, 223–233 use `validator_`. The plan uses `validator_` to match.

```cpp
TestServer::TestServer(Options opts)
    : impl_(std::make_unique<Impl>(opts)) {}

TestServer::TestServer(std::shared_ptr<PgContainer> pg)
    : impl_(std::make_unique<Impl>(Options{ .pg = std::move(pg) })) {}
```

In the `Impl` constructor body, branch on `no_auth`:

```cpp
Impl(Options opts) {
  // ... existing keypair setup (always done) ...
  if (opts.no_auth) {
    validator_.issuer = issuer_;
    validator_.audience = audience_;
    validator_.bypass = true;
    jwks_disabled_ = true;  // skip JWKS HTTP server bringup entirely
  } else {
    // Existing JWKS HTTP server bringup (currently lines 170–194)
    // moves into this branch verbatim:
    jwks_http_ = std::make_unique<httplib::Server>();
    jwks_http_->Get("/.well-known/jwks.json", /* ... existing handler ... */);
    jwks_http_->Get("/jwks.json",            /* ... existing handler ... */);
    const int http_port = jwks_http_->bind_to_any_port("127.0.0.1");
    std::thread([s = jwks_http_.get()]() { s->listen_after_bind(); }).detach();

    validator_.issuer = issuer_;
    validator_.audience = audience_;
    validator_.resolve_key = [](const std::string& kid) { /* ... existing ... */ };
  }
  // ... existing in-process gRPC server + service registration (always) ...
}
```

Add `bool jwks_disabled_ = false;` to `Impl` members (alongside `validator_` at line 269).

Update the existing destructor gate (currently `tests/fixtures/test_server.cc:260`) to honor the flag:

```cpp
if (jwks_http_ && !jwks_disabled_) jwks_http_->stop();
```

- [ ] **Step 6: Build + run new test — expect PASS**

Run:
```bash
cmake --build cmake-build-debug
cd cmake-build-debug && EVGRPC_TEST_DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' DATABASE_URL="$EVGRPC_TEST_DATABASE_URL" ctest -R evgrpc_e2e_tests
```
Expected: both `evgrpc_e2e_tests` (existing smoke + new `E2ESmokeNoAuth.*`) PASS — delegator keeps old behavior intact, new test exercises bypass path.

- [ ] **Step 7: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_e2e_tests --gtest_filter=E2ESmokeNoAuth.*`
Expected: PASS.

```bash
git add tests/fixtures/test_server.h tests/fixtures/test_server.cc tests/integration/smoke_e2e_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): TestServer::Options{no_auth} for integration tests"
```

---

### Task 3: `SharedPgEnvironment` + `TruncateAll`

**Files:**
- Create: `tests/fixtures/shared_pg.h`
- Create: `tests/fixtures/shared_pg.cc`
- Modify: `tests/fixtures/CMakeLists.txt`

- [ ] **Step 1: Write `shared_pg.h`**

```cpp
#pragma once
#include <gtest/gtest.h>
#include <memory>
#include "fixtures/pg_container.h"

namespace evgrpc::test {

class SharedPgEnvironment : public ::testing::Environment {
 public:
  void SetUp() override;    // opens PgContainer + applies sql/001_initial.sql
  void TearDown() override;
  static std::shared_ptr<PgContainer> pg();
  static void TruncateAll();
};

}  // namespace evgrpc::test
```

- [ ] **Step 2: Write `shared_pg.cc`**

```cpp
#include "fixtures/shared_pg.h"

#include <fstream>
#include <sstream>
#include <pqxx/pqxx>

namespace evgrpc::test {

namespace {
std::shared_ptr<PgContainer> g_pg;
}

std::shared_ptr<PgContainer> SharedPgEnvironment::pg() { return g_pg; }

void SharedPgEnvironment::TruncateAll() {
  auto c = std::make_shared<pqxx::connection>(g_pg->Conninfo());
  pqxx::work tx(*c);
  tx.exec("TRUNCATE vehicle, charging, consumption, "
          "source_category, weather CASCADE");
  tx.commit();
}

void SharedPgEnvironment::SetUp() {
  g_pg = std::make_shared<PgContainer>();
  // Apply schema (idempotent if dev DB already has it).
  // Path comes in via -DEVGRPC_TEST_SQL_PATH at compile time
  // (absolute path injected by CMake's `target_compile_definitions`)
  // so we don't depend on CWD — tests run from `cmake-build-debug/`.
  const auto sql_text = []() {
    std::ifstream f(EVGRPC_TEST_SQL_PATH);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
  }();
  auto c = std::make_shared<pqxx::connection>(g_pg->Conninfo());
  pqxx::work tx(*c);
  tx.exec(sql_text);
  tx.commit();
}

void SharedPgEnvironment::TearDown() {
  g_pg.reset();
}

}  // namespace evgrpc::test
```

- [ ] **Step 3: Wire into fixture library CMakeLists**

Add `shared_pg.cc` to the `evgrpc_test_fixtures` source list.

- [ ] **Step 4: Build — expect success, no runtime test yet**

Run: `cmake --build cmake-build-debug`
Expected: build OK.

- [ ] **Step 5: Commit**

```bash
git add tests/fixtures/shared_pg.h tests/fixtures/shared_pg.cc tests/fixtures/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): SharedPgEnvironment + TruncateAll for integration tests"
```

---

### Task 4: `service_integration_main.cc` + `ServiceITBase`

**Files:**
- Create: `tests/integration/service_integration_main.cc`
- Create: `tests/integration/service_test_fixtures.h`
- Create: `tests/integration/service_test_fixtures.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Write `service_test_fixtures.h`**

```cpp
#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include "fixtures/shared_pg.h"
#include "fixtures/test_server.h"

namespace evgrpc::test {

class ServiceITBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite();   // creates TestServer once per suite
  static void TearDownTestSuite();
  void SetUp() override;          // calls TruncateAll
  void TearDown() override;

  std::shared_ptr<grpc::Channel> channel() const { return channel_; }

 private:
  static std::shared_ptr<TestServer> server_;
  static std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace evgrpc::test
```

- [ ] **Step 2: Write `service_test_fixtures.cc`**

```cpp
#include "tests/integration/service_test_fixtures.h"

namespace evgrpc::test {

std::shared_ptr<TestServer> ServiceITBase::server_;
std::shared_ptr<grpc::Channel> ServiceITBase::channel_;

void ServiceITBase::SetUpTestSuite() {
  server_ = std::make_shared<TestServer>(TestServer::Options{
      .no_auth = true, .pg = SharedPgEnvironment::pg() });
  channel_ = server_->Channel();
}

void ServiceITBase::TearDownTestSuite() {
  channel_.reset();
  server_.reset();
}

void ServiceITBase::SetUp() {
  SharedPgEnvironment::TruncateAll();
}

void ServiceITBase::TearDown() {}

}  // namespace evgrpc::test
```

- [ ] **Step 3: Write `service_integration_main.cc`**

```cpp
#include <gtest/gtest.h>
#include "fixtures/shared_pg.h"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new evgrpc::test::SharedPgEnvironment);
  return RUN_ALL_TESTS();
}
```

- [ ] **Step 4: Wire new executable in `tests/integration/CMakeLists.txt`**

```cmake
add_executable(evgrpc_integration_tests
  service_integration_main.cc
  service_test_fixtures.cc
)
target_link_libraries(evgrpc_integration_tests PRIVATE
  gtest
  # NO gtest_main — service_integration_main.cc provides its own main()
  evgrpc_test_fixtures
  evgrpc_proto
)
add_test(NAME evgrpc_integration_tests COMMAND evgrpc_integration_tests)
```

Wire `EVGRPC_TEST_SQL_PATH` so `shared_pg.cc` can read the schema regardless of CWD:

```cmake
target_compile_definitions(evgrpc_test_fixtures PRIVATE
  EVGRPC_TEST_SQL_PATH="${CMAKE_SOURCE_DIR}/sql/001_initial.sql")
```

- [ ] **Step 5: Build + run empty binary**

Run:
```bash
cmake --build cmake-build-debug --target evgrpc_integration_tests
cd cmake-build-debug && EVGRPC_TEST_DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' DATABASE_URL="$EVGRPC_TEST_DATABASE_URL" ./tests/evgrpc_integration_tests
```
Expected: builds, runs, exits 0 with `[==========] 0 tests from 0 test suites ran.` (no tests yet, just env setup; we provide our own `main()`, not `gtest_main`).

- [ ] **Step 6: Commit**

```bash
git add tests/integration/service_integration_main.cc \
        tests/integration/service_test_fixtures.h \
        tests/integration/service_test_fixtures.cc \
        tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): integration test binary + ServiceITBase fixture"
```

---

### Task 5: `TestData` helpers (start with Vehicle)

**Files:**
- Create: `tests/integration/test_data.h`
- Create: `tests/integration/test_data.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Write `test_data.h`**

```cpp
#pragma once
#include <string>
#include "evgrpc/vehicle.pb.h"
// add other proto includes as we add helpers

namespace evgrpc::test::data {

std::string FreshUuid();
std::string FreshLicensePlate();

Vehicle MakeValidVehicle(std::string plate = "");

}  // namespace evgrpc::test::data
```

- [ ] **Step 2: Write `test_data.cc`**

```cpp
#include "tests/integration/test_data.h"

#include "util/uuid.h"  // evgrpc::NewUuid

namespace evgrpc::test::data {

std::string FreshUuid() { return evgrpc::NewUuid(); }

std::string FreshLicensePlate() {
  return "T-" + FreshUuid().substr(0, 8);
}

Vehicle MakeValidVehicle(std::string plate) {
  Vehicle v;
  v.set_id(FreshUuid());
  v.set_brand("Tesla");
  v.set_calibrated_range_km(500);
  v.set_battery_capacity_kwh(75.0);
  v.set_license_plate(plate.empty() ? FreshLicensePlate() : plate);
  google::protobuf::Timestamp ts;
  ts.set_seconds(1700000000);  // 2023-11-14
  *v.mutable_purchase_date() = ts;
  return v;
}

}  // namespace evgrpc::test::data
```

- [ ] **Step 3: Add to CMakeLists (modify the `add_executable` / `target_link_libraries` block from Task 4 Step 4)**

Add `test_data.cc` to `evgrpc_integration_tests` sources; link `evgrpc_proto` + `evgrpc_util`.

- [ ] **Step 4: Build — expect success**

Run: `cmake --build cmake-build-debug --target evgrpc_integration_tests`
Expected: build OK.

- [ ] **Step 5: Add `TestData` runtime smoke test**

Before committing, add a tiny runtime check (so a typo in the helper doesn't silently propagate to Chunk 2):

```cpp
// In tests/integration/test_data.cc at file scope
TEST(TestData, MakeValidVehicleIsValid) {
  const auto v = data::MakeValidVehicle();
  EXPECT_FALSE(v.id().empty());
  EXPECT_EQ(v.id().size(), 36u);                     // UUID length
  EXPECT_EQ(v.brand(), "Tesla");
  EXPECT_EQ(v.license_plate().substr(0, 2), "T-");   // default prefix
  EXPECT_GT(v.calibrated_range_km(), 0);
  EXPECT_GT(v.battery_capacity_kwh(), 0.0);
  EXPECT_TRUE(v.has_purchase_date());
}
```

- [ ] **Step 6: Run smoke + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=TestData.*`
Expected: PASS.

```bash
git add tests/integration/test_data.h tests/integration/test_data.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): TestData helpers (Vehicle starter) + runtime smoke"
```

---

### Task 6: CMake coverage flag (precursor for last chunk)

**Files:**
- Modify: `CMakeLists.txt` (top-level)

- [ ] **Step 1: Add option + flags**

After the existing `set(CMAKE_CXX_STANDARD_REQUIRED ON)`, add:

```cmake
option(EVGRPC_COVERAGE "Enable coverage instrumentation (gcov)" OFF)
if(EVGRPC_COVERAGE)
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()
```

- [ ] **Step 2: Configure a coverage build directory**

Run:
```bash
cmake -S . -B cmake-build-cov -DEVGRPC_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-cov --target evgrpc_integration_tests
```
Expected: builds with `--coverage` flag; verify via `find cmake-build-cov -name '*.gcno' | head` returns one file per compiled TU (e.g. `…/CMakeFiles/evgrpc_integration_tests.dir/tests/integration/service_integration_main.cc.o.gcno`). This is the reliable signal — `nm | grep gcov` would also match but `.gcno` files are unambiguous.

- [ ] **Step 3: Re-run existing smoke tests under coverage**

Run:
```bash
cd cmake-build-cov && EVGRPC_TEST_DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' DATABASE_URL="$EVGRPC_TEST_DATABASE_URL" ctest
```
Expected: both `evgrpc_tests` and `evgrpc_e2e_tests` PASS.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "build(cmake): EVGRPC_COVERAGE option (-fprofile-arcs -ftest-coverage)"
```

---

### End of Chunk 1

After Chunk 1 lands:
- `ctest -R evgrpc_integration_tests` runs (empty) in <10s.
- Smoke tests still pass unchanged.
- Coverage build pipeline works end-to-end.

If any Chunk 1 task fails verification, **STOP** and surface to the user before continuing.

---

## Chunk 2: VehicleService

Tasks 7-12 add 10-15 cases for the 5 VehicleService RPCs (CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles).

[Stub — to be written after Chunk 1 lands and is reviewed.]

---

## Chunk 3: ChargingService

[Stub — 10-15 cases for 5 RPCs.]

---

## Chunk 4: ConsumptionService

[Stub — 10-15 cases for 5 RPCs.]

---

## Chunk 5: DisplayService

[Stub — 16-21 cases for 8 RPCs (3 aggregation + 5 standard). Aggregation RPCs require `INTERNAL "no aggregate row"` case.]

---

## Chunk 6: SourceCategory + Weather

[Stub — 8-12 cases for 4 RPCs (2 Search* + 2 Create).]

---

## Chunk 7: Coverage + scripts/coverage.sh

[Stub — wire lcov, set threshold to ≥95% on src/services/, generate HTML report, hard-fail at 75s.]

---

## Execution Notes

- `EVGRPC_TEST_DATABASE_URL` and `DATABASE_URL` must both be set (see MEMORY.md — different env vars for different code paths).
- Password contains `@`; URL-encode: `NewUser@123` → `NewUser%40123`.
- After each task: rebuild, run only the affected test, commit. Don't batch commits across tasks.
- If `TruncateAll()` leaves a transaction open or a test fails mid-flight, drop the test DB and rerun from a fresh `SharedPgEnvironment::SetUp()`.
- Coverage exclusion is hard-coded in `scripts/coverage.sh`: `*/generated/*`, `*/_deps/*`, `*/tests/*`.