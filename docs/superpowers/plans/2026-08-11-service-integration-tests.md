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

Add to `tests/unit/test_jwt_validator.cc`. **Note:** the existing `JwtValidatorTest` suite uses `TEST_F` with a fixture whose `resolve_key` lambda would mask the negative case here. The new tests use `TEST` (no fixture) under a separate suite name to avoid that pollution; either `JwtValidatorTest` (rejected — gtest forbids mixing `TEST` and `TEST_F` under the same suite) or `JwtValidatorNoFixture` (chosen at implementation). The pre-existing 6 `JwtValidatorTest` tests are untouched.

```cpp
TEST(JwtValidatorNoFixture, BypassDefaultsToFalse) {
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
- Modify: `src/auth/authenticate.cc` (architectural gap — see Note below)
- Test: existing `evgrpc_e2e_tests` (smoke must still pass)

**Architectural gap (caught by implementer at 0bbff6d):** `AuthenticateRpc(ctx, *validator_, method)` calls `evgrpc::Authenticate(...)` which checks the `authorization` header BEFORE `JwtValidator.Validate(token)`. In `no_auth` mode the test calls `grpc::ClientContext ctx;` with NO credentials, so `Authenticate()` rejects with `UNAUTHENTICATED: missing authorization header` before `JwtValidator.bypass` can fire. Fix: `Authenticate()` early-returns synthesized claims when `validator.bypass` is set (READS the flag — does NOT write `bypass = true`). New `kReasonBypass` reason code added. The §10.5 grep gate is unaffected because this change READS, never WRITES, the literal.

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
- Modify: `tests/fixtures/CMakeLists.txt` (add `shared_pg.cc` to source list + 2 build-enabling additions per "Build deps" below)

**Build deps (architectural gaps caught by implementer at commit `f99c91a`):**

1. **`gtest` PUBLIC link on `evgrpc_test_fixtures`**: `shared_pg.h` derives `SharedPgEnvironment` from `::testing::Environment` (in `<gtest/gtest.h>`). Forward-declaration won't work — virtual override vtable needs the full type at link time. Add `gtest` to the existing PUBLIC link block alongside `httplib`, `grpc++`, etc.

2. **`EVGRPC_TEST_SQL_PATH` compile def on `evgrpc_test_fixtures` PUBLIC**: `shared_pg.cc` references the macro at compile time, so the build depends on it being defined somewhere. The plan originally reserved this for Task 4 Step 4, but Task 3 can't build without it. Add `target_compile_definitions(evgrpc_test_fixtures PUBLIC EVGRPC_TEST_SQL_PATH="${CMAKE_SOURCE_DIR}/sql/001_initial.sql")` here. Task 4's later wiring on its own target becomes redundant (PUBLIC propagates the def).

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

13 cases across 8 Tasks (1 precursor + 1 helper + 5 RPCs + 1 coverage). Specifically: Task 7 (precursor) + Task 8 (helper) + Tasks 9–13 (5 RPCs: CreateVehicle ×3, GetVehicle ×2, UpdateVehicle ×3, DeleteVehicle ×2, ListVehicles ×3) + Task 14 (lcov verification). Builds on Chunk 1's `ServiceITBase`. Adds `data::MakeValidCreateVehicleRequest()` (note: returns `CreateVehicleRequest`, **not** `Vehicle`) and a precursor production-code change in `src/db/error.cc`.

### Proto field reference (verified ground truth)

`proto/evgrpc/vehicle.proto`:
- `Vehicle { id, brand, calibrated_range_km, battery_capacity_kwh, purchase_date, license_plate }` (response)
- `CreateVehicleRequest { brand, calibrated_range_km, battery_capacity_kwh, purchase_date, license_plate }` (request — no id; different type from Vehicle)
- `GetVehicleRequest { id }`
- `UpdateVehicleRequest { id, brand, calibrated_range_km, battery_capacity_kwh, purchase_date, license_plate }` (fields flat — **no** `mutable_vehicle()`)
- `DeleteVehicleRequest { id }` (response is `google.protobuf.Empty`)
- `ListVehiclesRequest { page_size, page_token }` (response is `ListVehiclesResponse { repeated vehicles, next_page_token }`)

---

### Task 7 (precursor): Map `not_null_violation` → `INVALID_ARGUMENT` in `src/db/error.cc`

**Why this is first:** Empty-license-plate cases (Create + Update) hit the DB `NOT NULL` constraint, which throws pqxx `not_null_violation`. Current `ToGrpcStatus` falls through to `sql_error` → `INTERNAL`. Tests expecting `INVALID_ARGUMENT` will fail. Fix this once at the error-mapping layer so all services get consistent behavior.

**Files:**
- Modify: `src/db/error.cc:5-22`
- Modify: `tests/unit/test_error.cc` (add not_null_violation test)

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_error.cc`:

```cpp
#include <pqxx/pqxx>

TEST(ToGrpcStatus, NotNullViolationMapsToInvalidArgument) {
  // Construct a not_null_violation directly. pqxx 7.x constructor is
  // (err, Q, sqlstate); we only need the dynamic_cast path, so the
  // extra fields default to empty — passing `"23502"` as the second
  // arg would land in the `Q` (query) slot, not `sqlstate`.
  pqxx::not_null_violation e(
      "null value in column \"x\" violates not-null constraint");
  grpc::Status st = evgrpc::ToGrpcStatus(e);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 2: Run — expect FAIL**

Run: `cmake --build cmake-build-debug --target evgrpc_tests && cmake-build-debug/tests/evgrpc_tests --gtest_filter=ToGrpcStatus.NotNullViolationMapsToInvalidArgument`
Expected: FAIL with `actual: 13 (INTERNAL)` vs expected `INVALID_ARGUMENT`.

- [ ] **Step 3: Add mapping to `src/db/error.cc`**

```cpp
grpc::Status ToGrpcStatus(const std::exception& e) {
    if (dynamic_cast<const pqxx::unique_violation*>(&e)) {
        return {grpc::StatusCode::ALREADY_EXISTS, e.what()};
    }
    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e) ||
        dynamic_cast<const pqxx::not_null_violation*>(&e)) {  // NEW
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    if (dynamic_cast<const pqxx::data_exception*>(&e)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    if (dynamic_cast<const pqxx::sql_error*>(&e)) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
    return {grpc::StatusCode::INTERNAL, e.what()};
}
```

- [ ] **Step 4: Run — expect PASS, then run full test suite for regressions**

Run:
```bash
cmake-build-debug/tests/evgrpc_tests --gtest_filter=ToGrpcStatus.*
cmake --build cmake-build-debug && cd cmake-build-debug && \
  EVGRPC_TEST_DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' \
  DATABASE_URL="$EVGRPC_TEST_DATABASE_URL" ctest
```
Expected: PASS; no regressions (mapping change is additive — only adds a class to the `INVALID_ARGUMENT` branch).

- [ ] **Step 5: Commit**

```bash
git add src/db/error.cc tests/unit/test_error.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "fix(db): map not_null_violation to INVALID_ARGUMENT (was INTERNAL)"
```

---

### Task 8: Update `test_data` helper to also return `CreateVehicleRequest`

**Files:**
- Modify: `tests/integration/test_data.h`
- Modify: `tests/integration/test_data.cc`

- [ ] **Step 1: Add `MakeValidCreateVehicleRequest()` declaration**

In `tests/integration/test_data.h`, alongside `MakeValidVehicle`:

```cpp
#include "evgrpc/vehicle.pb.h"  // CreateVehicleRequest
// ...
namespace evgrpc::test::data {

Vehicle MakeValidVehicle(std::string plate = "");
CreateVehicleRequest MakeValidCreateVehicleRequest(std::string plate = "");

}
```

- [ ] **Step 2: Implement in `test_data.cc`**

```cpp
CreateVehicleRequest MakeValidCreateVehicleRequest(std::string plate) {
  CreateVehicleRequest req;
  req.set_brand("Tesla");
  req.set_calibrated_range_km(500);
  req.set_battery_capacity_kwh(75.0);
  google::protobuf::Timestamp ts;
  ts.set_seconds(1700000000);
  *req.mutable_purchase_date() = ts;
  req.set_license_plate(plate.empty() ? FreshLicensePlate() : plate);
  return req;
}
```

- [ ] **Step 3: Add runtime smoke (add to `tests/integration/test_data.cc`, after the Chunk 1 `TEST(TestData, MakeValidVehicleIsValid)` smoke)**

```cpp
TEST(TestData, MakeValidCreateVehicleRequestIsValid) {
  const auto req = data::MakeValidCreateVehicleRequest();
  EXPECT_EQ(req.brand(), "Tesla");
  EXPECT_GT(req.calibrated_range_km(), 0);
  EXPECT_GT(req.battery_capacity_kwh(), 0.0);
  EXPECT_TRUE(req.has_purchase_date());
  EXPECT_EQ(req.license_plate().substr(0, 2), "T-");
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=TestData.MakeValidCreateVehicleRequestIsValid`
Expected: PASS.

```bash
git add tests/integration/test_data.h tests/integration/test_data.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): MakeValidCreateVehicleRequest helper"
```

---

### Task 9: `CreateVehicle` (3 cases)

**Files:**
- Create: `tests/integration/vehicle_service_test.cc`
- Modify: `tests/integration/CMakeLists.txt` (add `vehicle_service_test.cc`)

- [ ] **Step 1: Write happy-path test (uses MakeValidCreateVehicleRequest, not Vehicle)**

```cpp
TEST_F(VehicleServiceIT, CreateVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  const auto req = data::MakeValidCreateVehicleRequest();
  Vehicle resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateVehicle(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.license_plate(), req.license_plate());
}
```

- [ ] **Step 2: Add duplicate-license-plate ALREADY_EXISTS case**

```cpp
TEST_F(VehicleServiceIT, CreateVehicle_DuplicateLicensePlate_Conflict) {
  auto stub = VehicleService::NewStub(channel());
  const std::string plate = "DUP-" + data::FreshUuid().substr(0, 4);
  Vehicle r1; grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(plate), &r1).ok());
  Vehicle r2; grpc::ClientContext ctx2;
  grpc::Status st = stub->CreateVehicle(&ctx2, data::MakeValidCreateVehicleRequest(plate), &r2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}
```

- [ ] **Step 3: Add empty-license-plate INVALID_ARGUMENT case (relies on Task 7 mapping)**

```cpp
TEST_F(VehicleServiceIT, CreateVehicle_EmptyLicensePlate_InvalidArgument) {
  auto stub = VehicleService::NewStub(channel());
  auto req = data::MakeValidCreateVehicleRequest();
  req.set_license_plate("");  // triggers NOT NULL → INVALID_ARGUMENT via Task 7
  Vehicle resp; grpc::ClientContext ctx;
  grpc::Status st = stub->CreateVehicle(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=VehicleServiceIT.CreateVehicle_*`
Expected: 3/3 PASS (Task 7 mapping makes the empty-plate case work).

```bash
git add tests/integration/vehicle_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(vehicle): CreateVehicle — happy + duplicate-plate + empty-plate (INVALID_ARGUMENT)"
```

---

### Task 10: `GetVehicle` (2 cases)

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(VehicleServiceIT, GetVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created; grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(), &created).ok());
  GetVehicleRequest greq; greq.set_id(created.id());
  Vehicle got; grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->GetVehicle(&ctx2, greq, &got).ok());
  EXPECT_EQ(got.id(), created.id());
  EXPECT_EQ(got.license_plate(), created.license_plate());
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(VehicleServiceIT, GetVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  GetVehicleRequest req; req.set_id("00000000-0000-0000-0000-000000000000");
  Vehicle got; grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicle(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=VehicleServiceIT.GetVehicle_*`
Expected: 2/2 PASS.

```bash
git add tests/integration/vehicle_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(vehicle): GetVehicle — happy + not-found"
```

---

### Task 11: `UpdateVehicle` (3 cases) — field-by-field, no `mutable_vehicle()`

- [ ] **Step 1: Write happy-path test (per-field assignment)**

```cpp
TEST_F(VehicleServiceIT, UpdateVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created; grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(), &created).ok());
  UpdateVehicleRequest ureq;
  ureq.set_id(created.id());
  ureq.set_brand("Renault");
  ureq.set_calibrated_range_km(created.calibrated_range_km());
  ureq.set_battery_capacity_kwh(created.battery_capacity_kwh());
  *ureq.mutable_purchase_date() = created.purchase_date();
  ureq.set_license_plate(created.license_plate());
  Vehicle resp; grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->UpdateVehicle(&ctx2, ureq, &resp).ok());
  EXPECT_EQ(resp.brand(), "Renault");
  EXPECT_EQ(resp.id(), created.id());  // id preserved
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(VehicleServiceIT, UpdateVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  UpdateVehicleRequest ureq;
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  ureq.set_brand("Renault");
  ureq.set_license_plate("NF-1");
  Vehicle resp; grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateVehicle(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Add empty-license-plate INVALID_ARGUMENT case**

```cpp
TEST_F(VehicleServiceIT, UpdateVehicle_EmptyLicensePlate_InvalidArgument) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created; grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(), &created).ok());
  UpdateVehicleRequest ureq;
  ureq.set_id(created.id());
  ureq.set_brand("Renault");
  ureq.set_calibrated_range_km(created.calibrated_range_km());
  ureq.set_battery_capacity_kwh(created.battery_capacity_kwh());
  *ureq.mutable_purchase_date() = created.purchase_date();
  ureq.set_license_plate("");  // NOT NULL → INVALID_ARGUMENT via Task 7
  Vehicle resp; grpc::ClientContext ctx2;
  grpc::Status st = stub->UpdateVehicle(&ctx2, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=VehicleServiceIT.UpdateVehicle_*`
Expected: 3/3 PASS.

```bash
git add tests/integration/vehicle_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(vehicle): UpdateVehicle — happy + not-found + empty-plate (INVALID_ARGUMENT)"
```

---

### Task 12: `DeleteVehicle` (2 cases)

- [ ] **Step 1: Write happy-path test (with post-condition check via GetVehicle)**

```cpp
TEST_F(VehicleServiceIT, DeleteVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created; grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(), &created).ok());
  DeleteVehicleRequest dreq; dreq.set_id(created.id());
  google::protobuf::Empty empty_resp; grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->DeleteVehicle(&ctx2, dreq, &empty_resp).ok());
  // Confirm gone
  GetVehicleRequest greq; greq.set_id(created.id());
  Vehicle got; grpc::ClientContext ctx3;
  EXPECT_EQ(stub->GetVehicle(&ctx3, greq, &got).error_code(),
            grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(VehicleServiceIT, DeleteVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  DeleteVehicleRequest dreq; dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp; grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteVehicle(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=VehicleServiceIT.DeleteVehicle_*`
Expected: 2/2 PASS.

```bash
git add tests/integration/vehicle_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(vehicle): DeleteVehicle — happy + not-found"
```

---

### Task 13: `ListVehicles` (3 cases) — pagination covers `has_more` branch

- [ ] **Step 1: Write happy-path test (3 rows, no pagination)**

```cpp
TEST_F(VehicleServiceIT, ListVehicles_HappyPath_MultipleRows) {
  auto stub = VehicleService::NewStub(channel());
  for (int i = 0; i < 3; ++i) {
    Vehicle v; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateVehicle(&c, data::MakeValidCreateVehicleRequest(), &v).ok());
  }
  ListVehiclesRequest req;  // default page_size=50 → all rows in one page
  ListVehiclesResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListVehicles(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.vehicles_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());  // no more pages
}
```

- [ ] **Step 2: Add empty case**

```cpp
TEST_F(VehicleServiceIT, ListVehicles_Empty) {
  auto stub = VehicleService::NewStub(channel());
  ListVehiclesRequest req;
  ListVehiclesResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListVehicles(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.vehicles_size(), 0);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

- [ ] **Step 3: Add pagination case (exercises `has_more` branch in `vehicle_service.cc:202`)**

```cpp
TEST_F(VehicleServiceIT, ListVehicles_Pagination) {
  auto stub = VehicleService::NewStub(channel());
  // Insert 5 rows
  for (int i = 0; i < 5; ++i) {
    Vehicle v; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateVehicle(&c, data::MakeValidCreateVehicleRequest(), &v).ok());
  }
  // First page: page_size=2 → 2 rows + next_page_token
  ListVehiclesRequest req; req.set_page_size(2);
  ListVehiclesResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListVehicles(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.vehicles_size(), 2);
  EXPECT_FALSE(resp.next_page_token().empty());

  // Second page: use the token → 2 more rows
  ListVehiclesRequest req2; req2.set_page_size(2); req2.set_page_token(resp.next_page_token());
  ListVehiclesResponse resp2; grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->ListVehicles(&ctx2, req2, &resp2).ok());
  EXPECT_EQ(resp2.vehicles_size(), 2);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=VehicleServiceIT.ListVehicles_*`
Expected: 3/3 PASS.

```bash
git add tests/integration/vehicle_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(vehicle): ListVehicles — happy + empty + pagination (covers has_more branch)"
```

---

### Task 14: Verify VehicleService coverage ≥ 95%

- [ ] **Step 1: Run lcov filtered to vehicle_service.cc**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file vehicle.info \
       --include '*/src/services/vehicle_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary vehicle.info 2>&1 | grep -E 'lines|====='
```
Expected: `lines......: 95.0%` or higher on `vehicle_service.cc`.

- [ ] **Step 2: If below 95%, identify uncovered lines**

Run: `genhtml vehicle.info --output-directory vehicle_html && grep 'class="lineUncov"' vehicle_html/src/services/vehicle_service.cc.gcov.html | head`
Expected: 0 uncovered lines (or fewer than 5% of total).

- [ ] **Step 3: Commit coverage verification (no code change unless new tests added)**

```bash
git add docs/superpowers/plans/2026-08-11-service-integration-tests.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "plan(chunk2): VehicleService coverage verified ≥95%"
```

---

### End of Chunk 2

After Chunk 2 lands:
- `evgrpc_integration_tests` has 13 VehicleService cases (Create 3 + Get 2 + Update 3 + Delete 2 + List 3), all green.
- `vehicle_service.cc` coverage ≥ 95% (helper Tasks 7–8 + cases Tasks 9–13).
- 1 production-code change in `src/db/error.cc` (Task 7) consistent across all services.

If any Task fails verification, **STOP** and surface to the user before continuing.

## Chunk 3: ChargingService

13 cases across 7 Tasks (1 helper-extension + 5 RPCs + 1 coverage). ChargingService has FK dependencies on `vehicle` and `source_category` tables, so Chunk 3 adds FK-setup helpers to `data::` so each test can self-contain its preconditions.

### Proto field reference (verified ground truth)

`proto/evgrpc/charging.proto`:
- `Charging` (response) — 16 fields: `id, vehicle_id, start_time, end_time, start_percent, end_percent, start_mileage_km, end_mileage_km, kwh_charged, cost, electricity_unit_price, service_fee (DoubleValue nullable), charger_type (enum), source_category_id, location, remark`
- `CreateChargingRequest` — 15 fields (no `id`); same shape as `Charging` minus the response `id`
- `GetChargingRequest { id }` / `UpdateChargingRequest { id, ... }` (fields flat) / `DeleteChargingRequest { id }`
- `ListChargingsRequest { page_size, page_token }` (response has `repeated chargings` + `next_page_token`)

### Impl validation contract (verified from `src/services/charging_service.cc`)

`ValidateCharging()` returns `INVALID_ARGUMENT` for:
- `end_time.seconds() <= start_time.seconds()`
- `end_percent <= start_percent`
- `kwh_charged <= 0`
- `cost <= 0`

**Validator runs BEFORE DB access** (verified at `charging_service.cc:155-158`, `UpdateCharging` re-runs at `:182-189`).

**DB constraints** (from `sql/001_initial.sql`):
- `vehicle_id` and `source_category_id` are `NOT NULL` and `REFERENCES vehicle(Id)` / `REFERENCES source_category(Id)` (FK violation → `INVALID_ARGUMENT` via Chunk 2 Task 7 mapping).

---

### Task 15: Extend `data::` with FK-setup helpers + `MakeValidCreateChargingRequest`

**Files:**
- Modify: `tests/integration/test_data.h`
- Modify: `tests/integration/test_data.cc`

- [ ] **Step 1: Add declarations (using `std::shared_ptr<grpc::Channel>` consistently, no `TestServerChannel`)**

In `tests/integration/test_data.h`:

```cpp
#include <grpcpp/grpcpp.h>
#include <memory>
#include "evgrpc/vehicle.pb.h"
#include "evgrpc/source_category.pb.h"
#include "evgrpc/charging.pb.h"

namespace evgrpc::test::data {

// FK-setup helpers: insert a row via the gRPC service, return the id.
// All take a raw channel and use NewStub() (codebase convention).
std::string CreateVehicleId(std::shared_ptr<grpc::Channel> channel);
std::string CreateSourceCategoryId(std::shared_ptr<grpc::Channel> channel);
std::string CreateChargingId(
    std::shared_ptr<grpc::Channel> channel,
    const std::string& vehicle_id,
    const std::string& source_category_id);

CreateChargingRequest MakeValidCreateChargingRequest(
    const std::string& vehicle_id,
    const std::string& source_category_id);

// Convert a CreateChargingRequest into an UpdateChargingRequest
// (drops nothing — same shape, just adding a set_id() at the call site
// is cheaper than duplicating all 15 set_* calls). Helper used by
// Update tests to avoid the 15-field copy-paste.
UpdateChargingRequest ToUpdateChargingRequest(const CreateChargingRequest& src);
void CopyToUpdateRequest(const CreateChargingRequest& src,
                         UpdateChargingRequest* dst);

}
```

- [ ] **Step 2: Implement helpers in `test_data.cc`**

```cpp
std::string CreateVehicleId(std::shared_ptr<grpc::Channel> channel) {
  auto stub = VehicleService::NewStub(channel);
  const auto req = MakeValidCreateVehicleRequest();
  Vehicle resp; grpc::ClientContext ctx;
  CHECK(stub->CreateVehicle(&ctx, req, &resp).ok());
  return resp.id();
}

std::string CreateSourceCategoryId(std::shared_ptr<grpc::Channel> channel) {
  auto stub = SourceCategoryService::NewStub(channel);
  CreateSourceCategoryRequest req;
  req.set_name("SC-" + FreshUuid().substr(0, 8));
  SourceCategory resp; grpc::ClientContext ctx;
  CHECK(stub->CreateSourceCategory(&ctx, req, &resp).ok());
  return resp.id();
}

std::string CreateChargingId(
    std::shared_ptr<grpc::Channel> channel,
    const std::string& vehicle_id,
    const std::string& source_category_id) {
  auto stub = ChargingService::NewStub(channel);
  const auto req = MakeValidCreateChargingRequest(vehicle_id, source_category_id);
  Charging resp; grpc::ClientContext ctx;
  CHECK(stub->CreateCharging(&ctx, req, &resp).ok());
  return resp.id();
}

CreateChargingRequest MakeValidCreateChargingRequest(
    const std::string& vehicle_id,
    const std::string& source_category_id) {
  CreateChargingRequest req;
  req.set_vehicle_id(vehicle_id);
  google::protobuf::Timestamp start; start.set_seconds(1700000000);
  google::protobuf::Timestamp end;   end.set_seconds(1700003600);  // +1h
  *req.mutable_start_time() = start;
  *req.mutable_end_time() = end;
  req.set_start_percent(20);
  req.set_end_percent(80);
  req.set_start_mileage_km(10000);
  req.set_end_mileage_km(10100);
  req.set_kwh_charged(50.0);
  req.set_cost(75.0);
  req.set_electricity_unit_price(1.5);
  req.set_charger_type(CHARGER_TYPE_FAST);
  req.set_source_category_id(source_category_id);
  req.set_location("Home");
  return req;
}

void CopyToUpdateRequest(const CreateChargingRequest& src,
                         UpdateChargingRequest* dst) {
  dst->set_vehicle_id(src.vehicle_id());
  *dst->mutable_start_time() = src.start_time();
  *dst->mutable_end_time() = src.end_time();
  dst->set_start_percent(src.start_percent());
  dst->set_end_percent(src.end_percent());
  dst->set_start_mileage_km(src.start_mileage_km());
  dst->set_end_mileage_km(src.end_mileage_km());
  dst->set_kwh_charged(src.kwh_charged());
  dst->set_cost(src.cost());
  dst->set_electricity_unit_price(src.electricity_unit_price());
  if (src.has_service_fee()) *dst->mutable_service_fee() = src.service_fee();
  dst->set_charger_type(src.charger_type());
  dst->set_source_category_id(src.source_category_id());
  dst->set_location(src.location());
  dst->set_remark(src.remark());
}

UpdateChargingRequest ToUpdateChargingRequest(const CreateChargingRequest& src) {
  UpdateChargingRequest dst;
  CopyToUpdateRequest(src, &dst);
  return dst;
}
```

- [ ] **Step 3: Add runtime smoke (inside a `TEST_F` so it has access to `channel()`)**

```cpp
// In tests/integration/charging_service_test.cc, file scope (above the
// first TEST_F), with the other TEST_F's inheriting from ServiceITBase.
TEST_F(ChargingServiceIT, DataHelpers_ProduceValidIds) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  EXPECT_FALSE(vid.empty());
  EXPECT_EQ(vid.size(), 36u);
  EXPECT_FALSE(sid.empty());
  EXPECT_EQ(sid.size(), 36u);
  const auto cid = data::CreateChargingId(channel, vid, sid);
  EXPECT_FALSE(cid.empty());
  EXPECT_EQ(cid.size(), 36u);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=ChargingServiceIT.DataHelpers_ProduceValidIds`
Expected: PASS.

```bash
git add tests/integration/test_data.h tests/integration/test_data.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): ChargingService helpers + FK-setup + ToUpdateChargingRequest"
```

---

### Task 16: `CreateCharging` (3 cases)

**Files:**
- Create: `tests/integration/charging_service_test.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Write happy-path test (with FK setup via helpers)**

```cpp
class ChargingServiceIT : public ServiceITBase {};

TEST_F(ChargingServiceIT, CreateCharging_HappyPath) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  auto stub = ChargingService::NewStub(channel);
  const auto req = data::MakeValidCreateChargingRequest(vid, sid);
  Charging resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateCharging(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_EQ(resp.source_category_id(), sid);
}
```

- [ ] **Step 2: Add FK violation case**

```cpp
TEST_F(ChargingServiceIT, CreateCharging_InvalidVehicleId_InvalidArgument) {
  auto channel = channel();
  const auto sid = data::CreateSourceCategoryId(channel);  // real
  auto stub = ChargingService::NewStub(channel);
  auto req = data::MakeValidCreateChargingRequest(
      "00000000-0000-0000-0000-000000000000",  // non-existent vehicle
      sid);
  Charging resp; grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 3: Add validation failure case (kwh_charged <= 0 → INVALID_ARGUMENT)**

```cpp
TEST_F(ChargingServiceIT, CreateCharging_NonPositiveKwh_InvalidArgument) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  auto stub = ChargingService::NewStub(channel);
  auto req = data::MakeValidCreateChargingRequest(vid, sid);
  req.set_kwh_charged(0.0);  // validator catches before DB
  Charging resp; grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=ChargingServiceIT.CreateCharging_*`
Expected: 3/3 PASS.

```bash
git add tests/integration/charging_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): CreateCharging — happy + invalid-vehicle + non-positive-kwh"
```

---

### Task 17: `GetCharging` (2 cases)

- [ ] **Step 1: Write happy-path test (uses full helper chain)**

```cpp
TEST_F(ChargingServiceIT, GetCharging_HappyPath) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  const auto cid = data::CreateChargingId(channel, vid, sid);
  auto stub = ChargingService::NewStub(channel);
  GetChargingRequest greq; greq.set_id(cid);
  Charging got; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCharging(&ctx, greq, &got).ok());
  EXPECT_EQ(got.id(), cid);
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ChargingServiceIT, GetCharging_NotFound) {
  auto stub = ChargingService::NewStub(channel());
  GetChargingRequest req; req.set_id("00000000-0000-0000-0000-000000000000");
  Charging got; grpc::ClientContext ctx;
  grpc::Status st = stub->GetCharging(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/charging_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): GetCharging — happy + not-found"
```

---

### Task 18: `UpdateCharging` (3 cases) — uses `ToUpdateChargingRequest` helper

- [ ] **Step 1: Write happy-path test (uses CopyToUpdateRequest + set_id)**

```cpp
TEST_F(ChargingServiceIT, UpdateCharging_HappyPath) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  auto stub = ChargingService::NewStub(channel);
  // Create first to get a valid id
  Charging created; grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateCharging(&c1,
      data::MakeValidCreateChargingRequest(vid, sid), &created).ok());
  // Build update request from a fresh valid template + override kwh
  auto template_req = data::MakeValidCreateChargingRequest(vid, sid);
  template_req.set_kwh_charged(60.0);  // the change
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(template_req);
  ureq.set_id(created.id());
  Charging resp; grpc::ClientContext c2;
  ASSERT_TRUE(stub->UpdateCharging(&c2, ureq, &resp).ok());
  EXPECT_EQ(resp.kwh_charged(), 60.0);
  EXPECT_EQ(resp.id(), created.id());
}
```

- [ ] **Step 2: Add NOT_FOUND case (uses ToUpdateChargingRequest so validator passes)**

```cpp
TEST_F(ChargingServiceIT, UpdateCharging_NotFound) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  auto stub = ChargingService::NewStub(channel);
  // Build a fully-valid update request but with a non-existent id.
  // The validator (re-run inside UpdateCharging) passes because all
  // template fields are valid; the SQL UPDATE finds 0 rows → NOT_FOUND.
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(
      data::MakeValidCreateChargingRequest(vid, sid));
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  Charging resp; grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateCharging(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Add validation failure case (end_time <= start_time → INVALID_ARGUMENT)**

```cpp
TEST_F(ChargingServiceIT, UpdateCharging_EndTimeBeforeStart_InvalidArgument) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  auto stub = ChargingService::NewStub(channel);
  Charging created; grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateCharging(&c1,
      data::MakeValidCreateChargingRequest(vid, sid), &created).ok());
  // Build update request where end_time == start_time (validator rejects)
  auto template_req = data::MakeValidCreateChargingRequest(vid, sid);
  *template_req.mutable_end_time() = template_req.start_time();
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(template_req);
  ureq.set_id(created.id());
  Charging resp; grpc::ClientContext c2;
  grpc::Status st = stub->UpdateCharging(&c2, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

Run: `cmake-build-debug/tests/evgrpc_integration_tests --gtest_filter=ChargingServiceIT.UpdateCharging_*`
Expected: 3/3 PASS.

```bash
git add tests/integration/charging_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): UpdateCharging — happy + not-found + end-time-validation"
```

---

### Task 19: `DeleteCharging` (2 cases)

- [ ] **Step 1: Write happy-path test (with post-condition check via GetCharging)**

```cpp
TEST_F(ChargingServiceIT, DeleteCharging_HappyPath) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  const auto cid = data::CreateChargingId(channel, vid, sid);
  auto stub = ChargingService::NewStub(channel);
  DeleteChargingRequest dreq; dreq.set_id(cid);
  google::protobuf::Empty empty; grpc::ClientContext c1;
  ASSERT_TRUE(stub->DeleteCharging(&c1, dreq, &empty).ok());
  // Confirm gone
  GetChargingRequest greq; greq.set_id(cid);
  Charging got; grpc::ClientContext c2;
  EXPECT_EQ(stub->GetCharging(&c2, greq, &got).error_code(),
            grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ChargingServiceIT, DeleteCharging_NotFound) {
  auto stub = ChargingService::NewStub(channel());
  DeleteChargingRequest dreq; dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp; grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteCharging(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/charging_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): DeleteCharging — happy + not-found"
```

---

### Task 20: `ListChargings` (3 cases) — pagination covers `has_more`

- [ ] **Step 1: Write happy-path test (3 rows, no pagination)**

```cpp
TEST_F(ChargingServiceIT, ListChargings_HappyPath_MultipleRows) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(
        ChargingService::NewStub(channel)->CreateCharging(
            &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  auto stub = ChargingService::NewStub(channel);
  ListChargingsRequest req;
  ListChargingsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListChargings(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.chargings_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

- [ ] **Step 2: Add empty case**

```cpp
TEST_F(ChargingServiceIT, ListChargings_Empty) {
  auto stub = ChargingService::NewStub(channel());
  ListChargingsRequest req;
  ListChargingsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListChargings(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.chargings_size(), 0);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

- [ ] **Step 3: Add pagination case (exercises `has_more` branch)**

```cpp
TEST_F(ChargingServiceIT, ListChargings_Pagination) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  for (int i = 0; i < 5; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(
        ChargingService::NewStub(channel)->CreateCharging(
            &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  auto stub = ChargingService::NewStub(channel);
  // First page: page_size=2 → 2 rows + non-empty next_page_token
  ListChargingsRequest req1; req1.set_page_size(2);
  ListChargingsResponse resp1; grpc::ClientContext c1;
  ASSERT_TRUE(stub->ListChargings(&c1, req1, &resp1).ok());
  EXPECT_EQ(resp1.chargings_size(), 2);
  EXPECT_FALSE(resp1.next_page_token().empty());
  // Second page with token
  ListChargingsRequest req2;
  req2.set_page_size(2);
  req2.set_page_token(resp1.next_page_token());
  ListChargingsResponse resp2; grpc::ClientContext c2;
  ASSERT_TRUE(stub->ListChargings(&c2, req2, &resp2).ok());
  EXPECT_EQ(resp2.chargings_size(), 2);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/charging_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): ListChargings — happy + empty + pagination"
```

---

### Task 21: Verify ChargingService coverage ≥ 95%

- [ ] **Step 1: Run lcov filtered to charging_service.cc**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file charging.info \
       --include '*/src/services/charging_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary charging.info 2>&1 | grep -E 'lines|====='
```
Expected: `lines......: 95.0%` or higher on `charging_service.cc`.

- [ ] **Step 2: If below 95%, identify uncovered lines**

Run: `genhtml charging.info --output-directory charging_html && grep 'class="lineUncov"' charging_html/src/services/charging_service.cc.gcov.html | head`

- [ ] **Step 3: Commit coverage verification**

```bash
git add docs/superpowers/plans/2026-08-11-service-integration-tests.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "plan(chunk3): ChargingService coverage verified ≥95%"
```

---

### Task 22 (conditional): Coverage gap closure — only run if Task 21 shows < 95%

**Why this is conditional:** R2 reviewer estimated `charging_service.cc` coverage at ~84-88% from the 13 cases above — likely below the 95% threshold. The branches missed:

1. **`ListChargings` filter branches** (5 filters in impl, none exercised): `vehicle_id`, `start_after`, `start_before`, `charger_type`, `source_category_id`.
2. **Remaining `ValidateCharging` branches**: `cost <= 0`, `end_percent <= start_percent` (only `kwh_charged <= 0` covered).
3. **`RowToCharging` non-NULL branches**: `ServiceFee`, `Location`, `Remark` (all set to empty by default helper).

If `lcov --summary charging.info` reports < 95%, add the cases below:

- [ ] **Step 1: Add 3 filtered `ListChargings` cases**

```cpp
TEST_F(ChargingServiceIT, ListChargings_FilterByVehicleId) {
  auto channel = channel();
  const auto vid = data::CreateVehicleId(channel);
  const auto sid = data::CreateSourceCategoryId(channel);
  // Create 3 chargings on this vehicle + 1 on a different vehicle
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(channel)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  const auto other_vid = data::CreateVehicleId(channel);
  Charging other; grpc::ClientContext co;
  ASSERT_TRUE(ChargingService::NewStub(channel)->CreateCharging(
      &co, data::MakeValidCreateChargingRequest(other_vid, sid), &other).ok());
  // Filter by vehicle_id
  auto stub = ChargingService::NewStub(channel);
  ListChargingsRequest req; req.set_vehicle_id(vid);
  ListChargingsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListChargings(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.chargings_size(), 3);
}

TEST_F(ChargingServiceIT, ListChargings_FilterByChargerType) {
  // ... 2 chargings with CHARGER_TYPE_FAST + 1 with CHARGER_TYPE_SLOW, filter by FAST → 2
}

TEST_F(ChargingServiceIT, ListChargings_FilterByDateRange) {
  // ... 3 chargings with varying start_time, filter by start_after=mid → fewer
}
```

- [ ] **Step 2: Add 2 missing `ValidateCharging` cases to Task 16**

```cpp
TEST_F(ChargingServiceIT, CreateCharging_NonPositiveCost_InvalidArgument) {
  // ... make valid request, set cost = 0.0, expect INVALID_ARGUMENT
}

TEST_F(ChargingServiceIT, CreateCharging_EndPercentLteStart_InvalidArgument) {
  // ... make valid request, set end_percent = start_percent, expect INVALID_ARGUMENT
}
```

- [ ] **Step 3: Add a `MakeValidCreateChargingRequest` variant with non-nullable fields set**

Extend the helper (or add `MakeValidCreateChargingRequestFull`) that also calls `set_service_fee(2.5)`, `set_location("Home charger")`, `set_remark("overnight")`. Then add one test case that uses it + asserts the response has those fields round-tripped.

- [ ] **Step 4: Re-run coverage, expect ≥ 95%**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file charging.info \
       --include '*/src/services/charging_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary charging.info 2>&1 | grep -E 'lines|====='
```

- [ ] **Step 5: Commit coverage closure**

```bash
git add tests/integration/charging_service_test.cc tests/integration/test_data.h tests/integration/test_data.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(charging): coverage gap closure — filtered list + missing validation + nullable field round-trip"
```

---

### End of Chunk 3

After Chunk 3 lands:
- `evgrpc_integration_tests` has 13 ChargingService cases (Create 3 + Get 2 + Update 3 + Delete 2 + List 3), all green.
- `charging_service.cc` coverage ≥ 95% (possibly requiring Task 22 to run; expected ~92% before, ~96% after).
- `data::CreateVehicleId` / `CreateSourceCategoryId` / `CreateChargingId` / `ToUpdateChargingRequest` helpers available for Chunks 4+.

If any Task fails verification, **STOP** and surface to the user before continuing.

## Chunk 4: ConsumptionService

13 cases across 8 Tasks (1 helper-extension + 5 RPCs + 1 coverage + 1 conditional). ConsumptionService has FK dependencies on `vehicle` AND `weather` tables. **Implementation note**: `weather_id` is created via direct SQL (not via WeatherService) so this chunk has no cross-chunk dependency on the WeatherService (which is implemented in Chunk 6).

### Proto field reference (verified ground truth)

`proto/evgrpc/consumption.proto`:
- `Consumption` (response) — 14 fields: `id, vehicle_id, start, end, begin_percent, end_percent, begin_mileage_km, end_mileage_km, begin_range_km, end_range_km, highest_temperature_c, lowest_temperature_c, weather_id, remark`
- `CreateConsumptionRequest` — 13 fields (no `id`); same shape as `Consumption` minus the response `id`
- `GetConsumptionRequest { id }` / `UpdateConsumptionRequest { id, ... }` (fields flat) / `DeleteConsumptionRequest { id }`
- `ListConsumptionsRequest { page_size, page_token, vehicle_id, start_after, start_before }` (response has `repeated consumptions` + `next_page_token`; **3 filters**: `vehicle_id`, `start_after`, `start_before`)

### Impl validation contract (verified from `src/services/consumption_service.cc:5-22`)

`ValidateConsumption()` returns `INVALID_ARGUMENT` for:
- `end.seconds() <= start.seconds()`
- `end_percent >= begin_percent` (note: inverse of `Charging` which checks `end_percent <= start_percent`)
- `highest_temperature_c < lowest_temperature_c`

`UpdateConsumption` re-runs the same validator (verified in impl).

**DB constraints** (verified from `sql/001_initial.sql:26`):
- `vehicle_id` and `weather_id` are BOTH `NOT NULL` and `REFERENCES vehicle(Id)` / `REFERENCES weather(Id)` (FK violation → `INVALID_ARGUMENT` via Chunk 2 Task 7).

**Test pattern note**: `ServiceITBase::channel()` / `ServiceITBase::pg()` qualified calls will NOT compile — these are non-static instance methods. Tests must use bare `channel()` / `pg()` (implicit `this`). Same issue exists in Chunk 3; **Chunk 3 needs the same fix** (out of scope here, flagged in commit message).

---

### Task 23: Extend `data::` with `CreateWeatherId` (direct SQL) + `MakeValidCreateConsumptionRequest` + `ToUpdateConsumptionRequest` + auto-creating `CreateConsumptionId`

**Files:**
- Modify: `tests/integration/test_data.h`
- Modify: `tests/integration/test_data.cc`

- [ ] **Step 1: Add declarations**

In `tests/integration/test_data.h`:

```cpp
#include <pqxx/pqxx>
#include "evgrpc/consumption.pb.h"
#include "fixtures/pg_container.h"

namespace evgrpc::test::data {

// Direct-SQL weather row creator. Avoids cross-chunk dependency on
// WeatherService (Chunk 6). Returns the new weather Id.
std::string CreateWeatherId(std::shared_ptr<PgContainer> pg);

// Convenience: auto-creates a weather row, then a consumption row.
// Returns the consumption id. Use this in tests that don't need to
// assert on a specific weather_id.
std::string CreateConsumptionId(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id);

// Note: `weather_id` is REQUIRED (DB column is NOT NULL with FK).
// There is NO default value. Callers must supply a real weather id.
CreateConsumptionRequest MakeValidCreateConsumptionRequest(
    const std::string& vehicle_id,
    const std::string& weather_id);

void CopyToUpdateRequest(const CreateConsumptionRequest& src,
                         UpdateConsumptionRequest* dst);
UpdateConsumptionRequest ToUpdateConsumptionRequest(const CreateConsumptionRequest& src);
}
```

- [ ] **Step 2: Add `pg()` accessor on `ServiceITBase`**

In `tests/integration/service_test_fixtures.h`:

```cpp
std::shared_ptr<PgContainer> pg() const { return SharedPgEnvironment::pg(); }
```

- [ ] **Step 3: Implement helpers in `test_data.cc`**

```cpp
std::string CreateWeatherId(std::shared_ptr<PgContainer> pg) {
  // Direct SQL insert — avoids cross-chunk dependency on WeatherService.
  auto conn = std::make_shared<pqxx::connection>(pg->Conninfo());
  pqxx::work tx(*conn);
  const std::string id = NewUuid();
  // Match `sql/001_initial.sql:18-21` weather table schema:
  //   Id UUID PRIMARY KEY, Name VARCHAR(36) NOT NULL UNIQUE
  tx.exec_params(
      "INSERT INTO weather (Id, Name) VALUES ($1, $2)",
      id, "W-" + id.substr(0, 8));
  tx.commit();
  return id;
}

std::string CreateConsumptionId(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id) {
  const auto wid = CreateWeatherId(pg);  // auto-create FK prerequisite
  auto stub = ConsumptionService::NewStub(channel);
  const auto req = MakeValidCreateConsumptionRequest(vehicle_id, wid);
  Consumption resp; grpc::ClientContext ctx;
  CHECK(stub->CreateConsumption(&ctx, req, &resp).ok());
  return resp.id();
}

CreateConsumptionRequest MakeValidCreateConsumptionRequest(
    const std::string& vehicle_id,
    const std::string& weather_id) {
  CreateConsumptionRequest req;
  req.set_vehicle_id(vehicle_id);
  req.set_weather_id(weather_id);  // REQUIRED — no default
  google::protobuf::Timestamp start; start.set_seconds(1700000000);
  google::protobuf::Timestamp end;   end.set_seconds(1700003600);
  *req.mutable_start() = start;
  *req.mutable_end() = end;
  req.set_begin_percent(80);
  req.set_end_percent(20);  // end < begin (validator requirement)
  req.set_begin_mileage_km(10000);
  req.set_end_mileage_km(10100);
  req.set_begin_range_km(400);
  req.set_end_range_km(350);
  req.set_highest_temperature_c(25.0);
  req.set_lowest_temperature_c(10.0);
  return req;
}

void CopyToUpdateRequest(const CreateConsumptionRequest& src,
                         UpdateConsumptionRequest* dst) {
  dst->set_vehicle_id(src.vehicle_id());
  *dst->mutable_start() = src.start();
  *dst->mutable_end() = src.end();
  dst->set_begin_percent(src.begin_percent());
  dst->set_end_percent(src.end_percent());
  dst->set_begin_mileage_km(src.begin_mileage_km());
  dst->set_end_mileage_km(src.end_mileage_km());
  dst->set_begin_range_km(src.begin_range_km());
  dst->set_end_range_km(src.end_range_km());
  dst->set_highest_temperature_c(src.highest_temperature_c());
  dst->set_lowest_temperature_c(src.lowest_temperature_c());
  dst->set_weather_id(src.weather_id());
  dst->set_remark(src.remark());
}

UpdateConsumptionRequest ToUpdateConsumptionRequest(const CreateConsumptionRequest& src) {
  UpdateConsumptionRequest dst;
  CopyToUpdateRequest(src, &dst);
  return dst;
}
```

- [ ] **Step 4: Add runtime smoke**

```cpp
TEST_F(ConsumptionServiceIT, DataHelpers_ProduceValidIds) {
  const auto vid = data::CreateVehicleId(channel());
  EXPECT_FALSE(vid.empty());
  const auto wid = data::CreateWeatherId(pg());
  EXPECT_FALSE(wid.empty());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  EXPECT_FALSE(cid.empty());
}
```

- [ ] **Step 5: Run + commit**

```bash
git add tests/integration/test_data.h tests/integration/test_data.cc tests/integration/service_test_fixtures.h
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): ConsumptionService helpers + CreateWeatherId + auto-creating CreateConsumptionId (weather_id NOT NULL)"
```

---

### Task 24: `CreateConsumption` (3 cases)

**Files:**
- Create: `tests/integration/consumption_service_test.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Write happy-path test (explicit weather_id round-trip)**

```cpp
class ConsumptionServiceIT : public ServiceITBase {};

TEST_F(ConsumptionServiceIT, CreateConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  const auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  Consumption resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateConsumption(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_EQ(resp.weather_id(), wid);
}
```

- [ ] **Step 2: Add FK violation case (invalid vehicle_id → INVALID_ARGUMENT)**

```cpp
TEST_F(ConsumptionServiceIT, CreateConsumption_InvalidVehicleId_InvalidArgument) {
  const auto wid = data::CreateWeatherId(pg());  // real
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(
      "00000000-0000-0000-0000-000000000000", wid);  // non-existent vehicle
  Consumption resp; grpc::ClientContext ctx;
  grpc::Status st = stub->CreateConsumption(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 3: Add validation failure case (highest_temp < lowest_temp → INVALID_ARGUMENT)**

```cpp
TEST_F(ConsumptionServiceIT, CreateConsumption_HighestTempLtLowest_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  req.set_highest_temperature_c(5.0);
  req.set_lowest_temperature_c(20.0);  // highest < lowest → INVALID_ARGUMENT
  Consumption resp; grpc::ClientContext ctx;
  grpc::Status st = stub->CreateConsumption(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/consumption_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): CreateConsumption — happy + invalid-vehicle + temp-validation"
```

---

### Task 25: `GetConsumption` (2 cases)

- [ ] **Step 1: Write happy-path test (uses CreateConsumptionId helper)**

```cpp
TEST_F(ConsumptionServiceIT, GetConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  auto stub = ConsumptionService::NewStub(channel());
  GetConsumptionRequest greq; greq.set_id(cid);
  Consumption got; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumption(&ctx, greq, &got).ok());
  EXPECT_EQ(got.id(), cid);
  EXPECT_EQ(got.vehicle_id(), vid);
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ConsumptionServiceIT, GetConsumption_NotFound) {
  auto stub = ConsumptionService::NewStub(channel());
  GetConsumptionRequest req; req.set_id("00000000-0000-0000-0000-000000000000");
  Consumption got; grpc::ClientContext ctx;
  grpc::Status st = stub->GetConsumption(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/consumption_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): GetConsumption — happy + not-found"
```

---

### Task 26: `UpdateConsumption` (3 cases) — uses `ToUpdateConsumptionRequest`

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(ConsumptionServiceIT, UpdateConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  Consumption created; grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateConsumption(&c1,
      data::MakeValidCreateConsumptionRequest(vid, wid), &created).ok());
  auto template_req = data::MakeValidCreateConsumptionRequest(vid, wid);
  template_req.set_end_mileage_km(10200);  // the change
  UpdateConsumptionRequest ureq = data::ToUpdateConsumptionRequest(template_req);
  ureq.set_id(created.id());
  Consumption resp; grpc::ClientContext c2;
  ASSERT_TRUE(stub->UpdateConsumption(&c2, ureq, &resp).ok());
  EXPECT_EQ(resp.end_mileage_km(), 10200);
  EXPECT_EQ(resp.id(), created.id());
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ConsumptionServiceIT, UpdateConsumption_NotFound) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  UpdateConsumptionRequest ureq = data::ToUpdateConsumptionRequest(
      data::MakeValidCreateConsumptionRequest(vid, wid));
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  Consumption resp; grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateConsumption(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Add validation failure case**

```cpp
TEST_F(ConsumptionServiceIT, UpdateConsumption_TempValidation_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  Consumption created; grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateConsumption(&c1,
      data::MakeValidCreateConsumptionRequest(vid, wid), &created).ok());
  auto template_req = data::MakeValidCreateConsumptionRequest(vid, wid);
  template_req.set_highest_temperature_c(0.0);
  template_req.set_lowest_temperature_c(20.0);
  UpdateConsumptionRequest ureq = data::ToUpdateConsumptionRequest(template_req);
  ureq.set_id(created.id());
  Consumption resp; grpc::ClientContext c2;
  grpc::Status st = stub->UpdateConsumption(&c2, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/consumption_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): UpdateConsumption — happy + not-found + temp-validation"
```

---

### Task 27: `DeleteConsumption` (2 cases)

- [ ] **Step 1: Write happy-path test (with post-condition check via GetConsumption)**

```cpp
TEST_F(ConsumptionServiceIT, DeleteConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  auto stub = ConsumptionService::NewStub(channel());
  DeleteConsumptionRequest dreq; dreq.set_id(cid);
  google::protobuf::Empty empty; grpc::ClientContext c1;
  ASSERT_TRUE(stub->DeleteConsumption(&c1, dreq, &empty).ok());
  GetConsumptionRequest greq; greq.set_id(cid);
  Consumption got; grpc::ClientContext c2;
  EXPECT_EQ(stub->GetConsumption(&c2, greq, &got).error_code(),
            grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ConsumptionServiceIT, DeleteConsumption_NotFound) {
  auto stub = ConsumptionService::NewStub(channel());
  DeleteConsumptionRequest dreq; dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp; grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteConsumption(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/consumption_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): DeleteConsumption — happy + not-found"
```

---

### Task 28: `ListConsumptions` (3 cases) — pagination covers `has_more`

- [ ] **Step 1: Write happy-path test (3 rows, no pagination)**

```cpp
TEST_F(ConsumptionServiceIT, ListConsumptions_HappyPath_MultipleRows) {
  const auto vid = data::CreateVehicleId(channel());
  for (int i = 0; i < 3; ++i) {
    Consumption v; grpc::ClientContext c;
    ASSERT_TRUE(ConsumptionService::NewStub(channel())->CreateConsumption(
        &c, data::CreateConsumptionId(channel(), pg(), vid).empty()
              ? data::MakeValidCreateConsumptionRequest(vid, data::CreateWeatherId(pg()))
              : data::MakeValidCreateConsumptionRequest(vid, data::CreateWeatherId(pg())),
        &v).ok());
  }
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req;
  ListConsumptionsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListConsumptions(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.consumptions_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

(Implementer: the inline conditional above is a copy/paste artifact. Simplify to: in each loop iteration, create the consumption via the auto-helper and assert OK. Cleaner version below; pick this one.)

```cpp
TEST_F(ConsumptionServiceIT, ListConsumptions_HappyPath_MultipleRows) {
  const auto vid = data::CreateVehicleId(channel());
  for (int i = 0; i < 3; ++i) {
    const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
    EXPECT_FALSE(cid.empty());
  }
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req;
  ListConsumptionsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListConsumptions(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.consumptions_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

- [ ] **Step 2: Add empty case**

```cpp
TEST_F(ConsumptionServiceIT, ListConsumptions_Empty) {
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req;
  ListConsumptionsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListConsumptions(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.consumptions_size(), 0);
  EXPECT_TRUE(resp.next_page_token().empty());
}
```

- [ ] **Step 3: Add pagination case (exercises `has_more` branch)**

```cpp
TEST_F(ConsumptionServiceIT, ListConsumptions_Pagination) {
  const auto vid = data::CreateVehicleId(channel());
  for (int i = 0; i < 5; ++i) {
    const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
    EXPECT_FALSE(cid.empty());
  }
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req1; req1.set_page_size(2);
  ListConsumptionsResponse resp1; grpc::ClientContext c1;
  ASSERT_TRUE(stub->ListConsumptions(&c1, req1, &resp1).ok());
  EXPECT_EQ(resp1.consumptions_size(), 2);
  EXPECT_FALSE(resp1.next_page_token().empty());
  ListConsumptionsRequest req2;
  req2.set_page_size(2);
  req2.set_page_token(resp1.next_page_token());
  ListConsumptionsResponse resp2; grpc::ClientContext c2;
  ASSERT_TRUE(stub->ListConsumptions(&c2, req2, &resp2).ok());
  EXPECT_EQ(resp2.consumptions_size(), 2);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/consumption_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): ListConsumptions — happy + empty + pagination"
```

---

### Task 29: Verify ConsumptionService coverage ≥ 95%

- [ ] **Step 1: Run lcov filtered to consumption_service.cc**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file consumption.info \
       --include '*/src/services/consumption_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary consumption.info 2>&1 | grep -E 'lines|====='
```
Expected: `lines......: 95.0%` or higher on `consumption_service.cc`.

- [ ] **Step 2: If below 95%, identify uncovered lines**

Run: `genhtml consumption.info --output-directory consumption_html && grep 'class="lineUncov"' consumption_html/src/services/consumption_service.cc.gcov.html | head`

- [ ] **Step 3: Commit coverage verification**

```bash
git add docs/superpowers/plans/2026-08-11-service-integration-tests.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "plan(chunk4): ConsumptionService coverage verified ≥95%"
```

---

### Task 30 (conditional): Coverage gap closure — only run if Task 29 shows < 95%

**Why conditional:** Likely missed branches:

1. **`ListConsumptions` filter branches** (3 filters per impl): `vehicle_id`, `start_after`, `start_before`.
2. **Remaining `ValidateConsumption` branch**: `end <= start` (only temp validation covered).
3. **`RowToConsumption` non-default fields**: `remark` (with `weather_id` already exercised in Task 24 Step 1).

If coverage < 95%, add:

- [ ] **Step 1: Add 3 filtered `ListConsumptions` cases** (one per filter)

```cpp
TEST_F(ConsumptionServiceIT, ListConsumptions_FilterByVehicleId) {
  // ... create 2 on vid_a + 1 on vid_b, filter by vid_a → 2
}

TEST_F(ConsumptionServiceIT, ListConsumptions_FilterByStartAfter) {
  // ... 3 rows with varying start times, filter by start_after=mid → fewer
}

TEST_F(ConsumptionServiceIT, ListConsumptions_FilterByStartBefore) {
  // ... 3 rows, filter by start_before=mid → fewer
}
```

- [ ] **Step 2: Add 1 `end_le_start` validation case**

```cpp
TEST_F(ConsumptionServiceIT, CreateConsumption_EndLeStart_InvalidArgument) {
  // ... make valid request, set end = start (via set_seconds equal) → INVALID_ARGUMENT
}
```

- [ ] **Step 3: Add a `MakeValidCreateConsumptionRequest` variant with `remark` set**

Extend the helper (or add `MakeValidCreateConsumptionRequestFull`) that calls `set_remark("trip notes")`. Add one test that uses it and asserts the response round-trips `remark`.

- [ ] **Step 4: Re-run lcov; expect ≥ 95%**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file consumption.info \
       --include '*/src/services/consumption_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary consumption.info 2>&1 | grep -E 'lines|====='
```

- [ ] **Step 5: Commit coverage closure**

```bash
git add tests/integration/consumption_service_test.cc tests/integration/test_data.h tests/integration/test_data.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(consumption): coverage gap closure — filtered list + end-le-start validation + remark round-trip"
```

---

### End of Chunk 4

After Chunk 4 lands:
- `evgrpc_integration_tests` has 13 ConsumptionService cases (Create 3 + Get 2 + Update 3 + Delete 2 + List 3), all green.
- `consumption_service.cc` coverage ≥ 95%.
- `data::CreateWeatherId` (direct SQL) + `MakeValidCreateConsumptionRequest` (weather_id required) + `CreateConsumptionId` (auto-creates weather) + `ToUpdateConsumptionRequest` helpers available.

If any Task fails verification, **STOP** and surface to the user before continuing.

## Chunk 5: DisplayService

**22 cases across 11 Tasks** (1 impl-fix precursor + 1 helper-extension + 3 aggregation RPCs + 5 list-style RPCs + 1 coverage + 1 conditional). DisplayService is the most complex service — 8 RPCs with 3 distinct return patterns (single-record aggregation, list-of-aggregates, list-of-records). Case-count math: 6 (3 aggregation × 2) + 15 (5 list × 3) + 1 smoke = 22.

### Proto field reference (verified ground truth)

`proto/evgrpc/display.proto`:
- 3 single-record aggregation responses: `VehicleCostSummary`, `PeriodReport` (×2)
- 5 list-style responses: `GetCostByChargerTypeResponse` / `GetCostBySourceCategoryResponse` / `GetConsumptionEfficiencyResponse` / `GetRangeAccuracyResponse` / `GetTemperatureConsumptionCorrelationResponse`
- All request messages: `string vehicle_id` (optional) + `google.protobuf.Timestamp start_time, end_time` (3 of the 5)
- Monthly/Annual reports: `int32 year`, `int32 month` (monthly only), `string vehicle_id` (optional)

### Impl behavior contract (verified from `src/services/display_service.cc`)

**3 RPCs return `INTERNAL "no aggregate row"` when no data matches** (verified at lines 82, 160, 224):
- `GetVehicleCostSummary` (line 82)
- `GetMonthlyReport` (line 160)
- `GetAnnualReport` (line 224)

**5 RPCs return empty list** when no data matches (no INTERNAL):
- `GetCostByChargerType`, `GetCostBySourceCategory`, `GetConsumptionEfficiency`, `GetRangeAccuracy`, `GetTemperatureConsumptionCorrelation`

**Validators** (verified at lines 14, 95, 167, etc.): a few of the RPCs reject `INVALID_ARGUMENT` for invalid time ranges (start > end). Most don't validate — they pass to SQL.

**No FK checks** at service level for DisplayService — the queries are aggregate reads only, not writes.

---

### Precursor before Task 31: Make `INTERNAL "no aggregate row"` branch reachable in DisplayService aggregation RPCs

**Why this is needed:** The current impl (`src/services/display_service.cc:60-83, 140-160, 207-224`) uses CTE CROSS JOIN pattern (`WITH c AS (...) , k AS (...) SELECT c.total_cost, c.total_kwh, k.total_km FROM c, k`) which always returns 1 row even when no data matches. This means `result.empty()` is **never** true, so the `INTERNAL "no aggregate row"` branch (lines 82, 160, 224) is dead code through the public API. The spec §6 promised this branch would fire on empty data; the impl never lets it.

**Fix:** Add an `EXISTS` pre-check at the start of each of the 3 aggregation RPCs that returns `INTERNAL "no aggregate row"` early if no rows match the filter. The aggregation query that follows stays as-is.

- [ ] **Step 1: Modify `GetVehicleCostSummary` (`display_service.cc:36-105`)**

Insert **after** `pqxx::nontransaction tx(*conn);` and the `MaybeTimestamp` locals (around line 55–60, after the existing `auto start_ts = MaybeTimestamp(...)` / `auto end_ts = MaybeTimestamp(...)` lines), before the existing aggregation SELECT:

```cpp
auto exists = db::Exec(tx,
    "SELECT EXISTS(SELECT 1 FROM charging "
    "WHERE ($1 = '' OR VehicleId = $1) "
    "AND ($2::TIMESTAMP IS NULL OR StartTime >= $2) "
    "AND ($3::TIMESTAMP IS NULL OR StartTime <= $3)) AS has_data",
    req->vehicle_id(), start_ts, end_ts);
if (!exists.empty() && !exists[0][0].as<bool>()) {
  auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
  scope.set_status(s); return s;
}
```

- [ ] **Step 2: Modify `GetMonthlyReport` (`display_service.cc:107-177`)**

Insert **after** `pqxx::nontransaction tx(*conn);` (line 125) and before any `pqxx::params p;` / SQL string construction, mirroring Step 1's pattern:

```cpp
auto exists = db::Exec(tx,
    "SELECT EXISTS(SELECT 1 FROM charging "
    "WHERE EXTRACT(YEAR FROM StartTime) = $1 AND EXTRACT(MONTH FROM StartTime) = $2 "
    "AND ($3 = '' OR VehicleId = $3)) AS has_data",
    req->year(), req->month(), req->vehicle_id());
if (!exists.empty() && !exists[0][0].as<bool>()) {
  auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
  scope.set_status(s); return s;
}
```

- [ ] **Step 3: Modify `GetAnnualReport` (`display_service.cc:179-241`)**

Insert **after** `pqxx::nontransaction tx(*conn);` (line 197) and before any `pqxx::params p;` / SQL string construction, mirroring Step 1's pattern:

```cpp
auto exists = db::Exec(tx,
    "SELECT EXISTS(SELECT 1 FROM charging "
    "WHERE EXTRACT(YEAR FROM StartTime) = $1 "
    "AND ($2 = '' OR VehicleId = $2)) AS has_data",
    req->year(), req->vehicle_id());
if (!exists.empty() && !exists[0][0].as<bool>()) {
  auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
  scope.set_status(s); return s;
}
```

- [ ] **Step 4: Run existing unit tests + commit**

Run: `cmake --build cmake-build-debug && cd cmake-build-debug && ctest -R evgrpc_tests`
Expected: PASS (additive change — only adds a new branch).

```bash
git add src/services/display_service.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "fix(display): make INTERNAL 'no aggregate row' reachable via EXISTS pre-check in 3 aggregation RPCs"
```

---

### Task 31: Extend `data::` with time-range helpers + Display helpers

**Files:**
- Create: `tests/integration/display_service_test.cc` (header section)
- Modify: `tests/integration/test_data.h`
- Modify: `tests/integration/test_data.cc`

- [ ] **Step 1a: Test file header — include the fixture class declaration as actual code**

In `tests/integration/display_service_test.cc`, at file scope (above the first `TEST_F`):

```cpp
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"
#include "evgrpc/display.grpc.pb.h"
#include "evgrpc/display.pb.h"

namespace evgrpc::test {

class DisplayServiceIT : public ServiceITBase {};

// The closing `}  // namespace evgrpc::test` is added at the END of
// the file (Task 39 Step 4 commit) — do NOT close the namespace here.
// All TEST_Fs in Tasks 32-39 must live inside this namespace for
// `TEST_F(DisplayServiceIT, ...)` and `data::CreateVehicleId(...)` to resolve.
```

(Necessary because Task 31 Step 3 below and Tasks 32-39 all use `TEST_F(DisplayServiceIT, ...)`; without this declaration the file won't compile.)

- [ ] **Step 1: Add declarations**

In `tests/integration/test_data.h`:

```cpp
#include "evgrpc/display.pb.h"
// ...

namespace evgrpc::test::data {

// Time range — covers Nov 2023 (the seeded helper range) with margin.
// Helpers in Chunks 3/4 use start.set_seconds(1700000000) = 2023-11-14.
// Default range: 2023-01-01 to 2024-01-01, so any helper-generated row is included.
struct TimeRange {
  google::protobuf::Timestamp start;
  google::protobuf::Timestamp end;
};
TimeRange DefaultTimeRange();  // 2023-01-01 00:00:00 to 2024-01-01 00:00:00 UTC

// Aggregations need data to exist; provide a helper that inserts
// enough charging + consumption rows for one vehicle to make the
// aggregations non-empty.
void SeedVehicleDataForDisplay(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id,
    int n_chargings = 3,
    int n_consumptions = 3);
}
```

- [ ] **Step 2: Implement in `test_data.cc`**

```cpp
TimeRange DefaultTimeRange() {
  // Wide range covering any data the helpers (Chunks 3/4) seed.
  // Chunk 3/4 helpers use start.set_seconds(1700000000) = 2023-11-14.
  // Use 2023-01-01 to 2024-01-01 so any helper-generated row is included.
  TimeRange r;
  r.start.set_seconds(1672531200);   // 2023-01-01 00:00:00 UTC
  r.end.set_seconds(1704067200);     // 2024-01-01 00:00:00 UTC
  return r;
}

void SeedVehicleDataForDisplay(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id,
    int n_chargings,
    int n_consumptions) {
  const auto sid = CreateSourceCategoryId(channel);
  for (int i = 0; i < n_chargings; ++i) {
    const auto cid = CreateChargingId(channel, vehicle_id, sid);
    CHECK(!cid.empty()) << "CreateChargingId returned empty";
  }
  for (int i = 0; i < n_consumptions; ++i) {
    const auto cid = CreateConsumptionId(channel, pg, vehicle_id);
    CHECK(!cid.empty()) << "CreateConsumptionId returned empty";
  }
}
```

- [ ] **Step 3: Add runtime smoke**

```cpp
TEST_F(DisplayServiceIT, DataHelpers_ProduceValidSetup) {
  const auto vid = data::CreateVehicleId(channel());
  EXPECT_FALSE(vid.empty());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  // Stronger signal: re-create and verify all helpers return non-empty
  const auto cid = data::CreateChargingId(channel(), vid, data::CreateSourceCategoryId(channel()));
  EXPECT_FALSE(cid.empty());
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/test_data.h tests/integration/test_data.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(test): DisplayService helpers + time-range + SeedVehicleDataForDisplay"
```

---

### Task 32: `GetVehicleCostSummary` (2 cases) — INTERNAL when no data

- [ ] **Step 1: Write happy-path test (with seeded data)**

```cpp
// Fixture class declared at file scope (top of display_service_test.cc):
//   class DisplayServiceIT : public ServiceITBase {};
// (Declared in Task 31 Step 1a — see above.)

TEST_F(DisplayServiceIT, GetVehicleCostSummary_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  VehicleCostSummary resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetVehicleCostSummary(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_GT(resp.total_cost(), 0.0);
  EXPECT_GT(resp.total_kwh(), 0.0);
}
```

- [ ] **Step 2: Write INTERNAL case (no data → "no aggregate row")**

```cpp
TEST_F(DisplayServiceIT, GetVehicleCostSummary_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  // Note: do NOT seed data — empty DB
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  VehicleCostSummary resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicleCostSummary(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos)
      << st.error_message();
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/display_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetVehicleCostSummary — happy + no-data-INTERNAL"
```

---

### Task 33: `GetMonthlyReport` (2 cases) — INTERNAL when no data

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetMonthlyReport_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(2023);   // helper data is in Nov 2023
  req.set_month(11);
  req.set_vehicle_id(vid);  // optional
  PeriodReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetMonthlyReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.year(), 2023);
  EXPECT_EQ(resp.month(), 11);
  EXPECT_GT(resp.total_cost(), 0.0);
}
```

- [ ] **Step 2: Write INTERNAL case**

```cpp
TEST_F(DisplayServiceIT, GetMonthlyReport_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(2099);  // future year, no data
  req.set_month(1);
  req.set_vehicle_id(vid);
  PeriodReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetMonthlyReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetMonthlyReport — happy + no-data-INTERNAL"
```

---

### Task 34: `GetAnnualReport` (2 cases) — INTERNAL when no data

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetAnnualReport_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetAnnualReportRequest req;
  req.set_year(2023);   // helper data is in Nov 2023
  req.set_vehicle_id(vid);
  PeriodReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetAnnualReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.year(), 2023);
  EXPECT_EQ(resp.month(), 0);  // 0 = annual sentinel (proto comment: "0 = annual; 1-12 = monthly")
  EXPECT_GT(resp.total_cost(), 0.0);
}
```

- [ ] **Step 2: Write INTERNAL case**

```cpp
TEST_F(DisplayServiceIT, GetAnnualReport_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetAnnualReportRequest req;
  req.set_year(2099);
  req.set_vehicle_id(vid);
  PeriodReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetAnnualReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos);
}
```

- [ ] **Step 3: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetAnnualReport — happy + no-data-INTERNAL"
```

---

### Task 35: `GetCostByChargerType` (3 cases) — empty list, not INTERNAL

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetCostByChargerType_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  EXPECT_GT(resp.breakdowns_size(), 0);
  for (const auto& b : resp.breakdowns()) {
    EXPECT_GT(b.total_cost(), 0.0);
  }
}
```

- [ ] **Step 2: Write empty case (no data → empty list, NOT INTERNAL)**

```cpp
TEST_F(DisplayServiceIT, GetCostByChargerType_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}
```

- [ ] **Step 3: Write filtered case (vehicle_id filter excludes other vehicles)**

```cpp
TEST_F(DisplayServiceIT, GetCostByChargerType_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  // Unfiltered baseline — both vehicles' data
  GetCostByChargerTypeRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp_unf; grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx_unf, req_unf, &resp_unf).ok());
  double total_unf = 0;
  for (const auto& b : resp_unf.breakdowns()) total_unf += b.total_cost();
  // Filtered — vid_a only
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  double total_filt = 0;
  for (const auto& b : resp.breakdowns()) total_filt += b.total_cost();
  EXPECT_LT(total_filt, total_unf);  // filtered is strictly less than unfiltered
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetCostByChargerType — happy + empty + filtered"
```

---

### Task 36: `GetCostBySourceCategory` (3 cases)

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetCostBySourceCategory_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  EXPECT_GT(resp.breakdowns_size(), 0);
}
```

- [ ] **Step 2: Write empty case**

```cpp
TEST_F(DisplayServiceIT, GetCostBySourceCategory_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}
```

- [ ] **Step 3: Write filtered case**

```cpp
TEST_F(DisplayServiceIT, GetCostBySourceCategory_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  // Unfiltered baseline
  GetCostBySourceCategoryRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp_unf; grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx_unf, req_unf, &resp_unf).ok());
  double total_unf = 0;
  for (const auto& b : resp_unf.breakdowns()) total_unf += b.total_cost();
  // Filtered — vid_a only
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  double total_filt = 0;
  for (const auto& b : resp.breakdowns()) total_filt += b.total_cost();
  EXPECT_LT(total_filt, total_unf);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetCostBySourceCategory — happy + empty + filtered"
```

---

### Task 37: `GetConsumptionEfficiency` (3 cases)

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  EXPECT_GT(resp.efficiencies_size(), 0);
}
```

- [ ] **Step 2: Write empty case**

```cpp
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.efficiencies_size(), 0);
}
```

- [ ] **Step 3: Write filtered case**

```cpp
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  for (const auto& e : resp.efficiencies()) {
    EXPECT_EQ(e.vehicle_id(), vid_a);  // all rows are for vid_a
  }
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetConsumptionEfficiency — happy + empty + filtered"
```

---

### Task 38: `GetRangeAccuracy` (3 cases)

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetRangeAccuracy_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  EXPECT_GT(resp.accuracies_size(), 0);
}
```

- [ ] **Step 2: Write empty case**

```cpp
TEST_F(DisplayServiceIT, GetRangeAccuracy_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.accuracies_size(), 0);
}
```

- [ ] **Step 3: Write filtered case**

```cpp
TEST_F(DisplayServiceIT, GetRangeAccuracy_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  for (const auto& a : resp.accuracies()) {
    EXPECT_EQ(a.vehicle_id(), vid_a);
  }
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetRangeAccuracy — happy + empty + filtered"
```

---

### Task 39: `GetTemperatureConsumptionCorrelation` (3 cases)

- [ ] **Step 1: Write happy-path test**

```cpp
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  EXPECT_GT(resp.buckets_size(), 0);
}
```

- [ ] **Step 2: Write empty case**

```cpp
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.buckets_size(), 0);
}
```

- [ ] **Step 3: Write filtered case**

```cpp
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  // Unfiltered baseline — bucket totals across both vehicles
  GetTemperatureConsumptionCorrelationRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp_unf; grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx_unf, req_unf, &resp_unf).ok());
  int total_unf = 0;
  for (const auto& b : resp_unf.buckets()) total_unf += b.sample_count();
  // Filtered — vid_a only
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  int total_filt = 0;
  for (const auto& b : resp.buckets()) total_filt += b.sample_count();
  EXPECT_LT(total_filt, total_unf);
}
```

- [ ] **Step 4: Run + commit**

```bash
git add tests/integration/display_service_test.cc
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): GetTemperatureConsumptionCorrelation — happy + empty + filtered"
```

---

### Task 40: Verify DisplayService coverage ≥ 95%

- [ ] **Step 1: Run lcov filtered to display_service.cc**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file display.info \
       --include '*/src/services/display_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary display.info 2>&1 | grep -E 'lines|====='
```
Expected: `lines......: 95.0%` or higher on `display_service.cc`.

- [ ] **Step 2: If below 95%, identify uncovered lines**

Run: `genhtml display.info --output-directory display_html && grep 'class="lineUncov"' display_html/src/services/display_service.cc.gcov.html | head`

- [ ] **Step 3: Commit coverage verification**

```bash
git add docs/superpowers/plans/2026-08-11-service-integration-tests.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "plan(chunk5): DisplayService coverage verified ≥95%"
```

---

### Task 41 (conditional): Coverage gap closure — only run if Task 40 shows < 95%

**Why conditional:** DisplayService has 592 LOC, 8 RPCs, 5 list filters — likely missed branches:

1. **Filter branches in 5 list-style RPCs**: `start_time`/`end_time` filters (some RPCs have conditional WHERE clauses).
2. **Validator branches** (if `start > end` in some RPCs).
3. **`PeriodReport` total_km / total_kwh aggregation branches** (might have COALESCE / NULL handling).

If coverage < 95%, add (concrete, ordered by likely impact):

- [ ] **Step 1: Add 5 time-range filter cases** (one per list-style RPC) — narrow window excludes seeded data → returns empty
  ```cpp
  TEST_F(DisplayServiceIT, GetCostByChargerType_TimeRangeFilter_Empty) {
    const auto vid = data::CreateVehicleId(channel());
    data::SeedVehicleDataForDisplay(channel(), pg(), vid);
    auto stub = DisplayService::NewStub(channel());
    GetCostByChargerTypeRequest req;
    req.set_vehicle_id(vid);
    req.mutable_start_time()->set_seconds(1735689600);  // 2025-01-01
    req.mutable_end_time()->set_seconds(1735776000);    // 2025-01-02
    GetCostByChargerTypeResponse resp; grpc::ClientContext ctx;
    ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.breakdowns_size(), 0);
  }
  // Repeat for: GetCostBySourceCategory, GetConsumptionEfficiency, GetRangeAccuracy, GetTemperatureConsumptionCorrelation
  ```
- [ ] **Step 2: Add 1-2 INVALID_ARGUMENT cases** for RPCs with validators (check `display_service.cc` for `start_time >= end_time` checks):
  ```cpp
  TEST_F(DisplayServiceIT, GetVehicleCostSummary_StartAfterEnd_InvalidArgument) {
    // ... set start > end, expect INVALID_ARGUMENT (only if validator exists)
  }
  ```
- [ ] **Step 3: Add multi-vehicle case** for any list RPC that supports no `vehicle_id` filter (asserts > 1 row across multiple vehicles):
  ```cpp
  TEST_F(DisplayServiceIT, GetCostByChargerType_NoVehicleFilter) {
    const auto vid_a = data::CreateVehicleId(channel());
    const auto vid_b = data::CreateVehicleId(channel());
    data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
    data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
    // ... assert multiple breakdowns or larger total than filtered single-vehicle case
  }
  ```
- [ ] **Step 4: Re-run lcov; expect ≥ 95%**
  ```bash
  cd cmake-build-cov && \
    lcov --capture --directory . --output-file display.info \
         --include '*/src/services/display_service.cc' \
         --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
  lcov --summary display.info 2>&1 | grep -E 'lines|====='
  ```
- [ ] **Step 5: Commit coverage closure**
  ```bash
  git add tests/integration/display_service_test.cc
  git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(display): coverage gap closure — time-range filter + validator + multi-vehicle"
  ```

---

### End of Chunk 5

After Chunk 5 lands:
- `evgrpc_integration_tests` has **22 cases** (3 aggregation × 2 + 5 list × 3 + 1 helper smoke = 6 + 15 + 1), all green.
- `display_service.cc` coverage ≥ 95%; **28-29 cases** total if Task 41 (conditional coverage closure) runs.
- `data::DefaultTimeRange` + `SeedVehicleDataForDisplay` helpers available.

If any Task fails verification, **STOP** and surface to the user before continuing.

## Chunk 6: SourceCategory + Weather

8 cases across 4 Tasks (2 services × 1 Create + 1 Search with 3 sub-cases = 8). Both services are nearly identical (same shape, same DB table — `source_category` and `weather` both have `Id` + `Name` with `Name UNIQUE NOT NULL`). Chunk 6 groups them because the test patterns are identical.

### Proto field reference (verified ground truth)

`proto/evgrpc/source_category.proto`:
- `SourceCategory { id, name }`
- `CreateSourceCategoryRequest { string name }`
- `SearchSourceCategoryRequest { string prefix, int32 limit }` (response: `repeated SourceCategory matches`)
- 2 RPCs: `CreateSourceCategory`, `SearchSourceCategory`

`proto/evgrpc/weather.proto` (identical structure):
- `Weather { id, name }`
- `CreateWeatherRequest { string name }`
- `SearchWeatherRequest { string prefix, int32 limit }` (response: `repeated Weather matches`)
- 2 RPCs: `CreateWeather`, `SearchWeather`

### Impl behavior contract (verified from `src/services/source_category_service.cc:22-34` and `src/services/weather_service.cc:22-34`)

- **Create**: no client-side validation. Empty `name` is accepted and stored (DB column is `NOT NULL` so it can be empty string but not NULL). Inserts row with `NewUuid()` for `Id`.
- **Search**: LIKE-prefix filter on `Name`. Empty prefix returns all rows (or some default subset). `limit <= 0` defaults to 50. `limit > 0` caps result count.
- **No FK checks** — both services insert/select their own table only.

---

### Task 42: `CreateSourceCategory` (1 case)

**Files:**
- Create: `tests/integration/source_category_service_test.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Test file header — namespace + fixture class declaration**

```cpp
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"
#include "evgrpc/source_category.grpc.pb.h"
#include "evgrpc/source_category.pb.h"

namespace evgrpc::test {

class SourceCategoryServiceIT : public ServiceITBase {};

// Closing `}  // namespace evgrpc::test` added at end of file.
```

- [ ] **Step 2: Write happy-path test**

```cpp
TEST_F(SourceCategoryServiceIT, CreateSourceCategory_HappyPath) {
  auto stub = SourceCategoryService::NewStub(channel());
  CreateSourceCategoryRequest req;
  req.set_name("SC-" + data::FreshUuid().substr(0, 8));
  SourceCategory resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateSourceCategory(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.name(), req.name());
}
```

- [ ] **Step 3: Visual check (no build yet — namespace not yet closed)**

The file is intentionally not built at this step because the namespace block (opened in Step 1) is still open. Adding the closing brace happens at Task 43 Step 4 (alongside the 3 more TEST_Fs and the build). **No commit at this step.**

Visual check: the file should contain `namespace evgrpc::test { class SourceCategoryServiceIT : public ServiceITBase {};` followed by 1 `TEST_F(...)` body. The file should NOT have a closing `}` yet.

---

### Task 43: `SearchSourceCategory` (3 cases)

- [ ] **Step 1: Write happy-path test (with multiple rows)**

```cpp
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_HappyPath) {
  auto stub = SourceCategoryService::NewStub(channel());
  // Insert 3 rows with shared "Solar-" prefix
  for (int i = 0; i < 3; ++i) {
    CreateSourceCategoryRequest req;
    req.set_name("Solar-" + data::FreshUuid().substr(0, 8));
    SourceCategory resp; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateSourceCategory(&c, req, &resp).ok());
  }
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("Solar-");
  sreq.set_limit(50);
  SearchSourceCategoryResponse resp; grpc::ClientContext cx;
  ASSERT_TRUE(stub->SearchSourceCategory(&cx, sreq, &resp).ok());
  EXPECT_GE(resp.matches_size(), 3);  // at least the 3 we just inserted
}
```

- [ ] **Step 2: Write empty case (no matches)**

```cpp
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_Empty) {
  auto stub = SourceCategoryService::NewStub(channel());
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("ZZZZ-NoSuchPrefix-");
  sreq.set_limit(50);
  SearchSourceCategoryResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchSourceCategory(&ctx, sreq, &resp).ok());
  EXPECT_EQ(resp.matches_size(), 0);
}
```

- [ ] **Step 3: Write filtered case (limit caps results)**

```cpp
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_LimitCapped) {
  auto stub = SourceCategoryService::NewStub(channel());
  // Insert 5 rows with shared "Wind-" prefix
  for (int i = 0; i < 5; ++i) {
    CreateSourceCategoryRequest req;
    req.set_name("Wind-" + data::FreshUuid().substr(0, 8));
    SourceCategory resp; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateSourceCategory(&c, req, &resp).ok());
  }
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("Wind-");
  sreq.set_limit(2);  // cap at 2
  SearchSourceCategoryResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchSourceCategory(&ctx, sreq, &resp).ok());
  EXPECT_LE(resp.matches_size(), 2);
}
```

- [ ] **Step 4: Close namespace + build + run + commit (covers Tasks 42 + 43)**

Append at the bottom of `tests/integration/source_category_service_test.cc`:

```cpp
}  // namespace evgrpc::test
```

(Pattern matches Task 31 Step 1a / Task 39 Step 4 / Task 44 Step 4.)

Run:
```bash
cmake --build cmake-build-debug --target evgrpc_integration_tests
cd cmake-build-debug && ctest -R evgrpc_integration_tests --gtest_filter='SourceCategoryServiceIT.*'
```
Expected: 4/4 PASS (1 from Task 42 + 3 from Task 43).

```bash
git add tests/integration/source_category_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(source_category): CreateSourceCategory + SearchSourceCategory — happy + empty + limit-capped"
```

---

### Task 44: `CreateWeather` + `SearchWeather` (1 + 3 cases, same pattern as Tasks 42-43)

**Files:**
- Create: `tests/integration/weather_service_test.cc`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Test file header**

```cpp
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"
#include "evgrpc/weather.grpc.pb.h"
#include "evgrpc/weather.pb.h"

namespace evgrpc::test {

class WeatherServiceIT : public ServiceITBase {};

// Closing `}  // namespace evgrpc::test` added at end of file.
```

- [ ] **Step 2: Write happy-path test for `CreateWeather`**

```cpp
TEST_F(WeatherServiceIT, CreateWeather_HappyPath) {
  auto stub = WeatherService::NewStub(channel());
  CreateWeatherRequest req;
  req.set_name("W-" + data::FreshUuid().substr(0, 8));
  Weather resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateWeather(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.name(), req.name());
}
```

- [ ] **Step 3: Write 3 cases for `SearchWeather`** (mirror Tasks 43 pattern)

```cpp
TEST_F(WeatherServiceIT, SearchWeather_HappyPath) {
  auto stub = WeatherService::NewStub(channel());
  for (int i = 0; i < 3; ++i) {
    CreateWeatherRequest req;
    req.set_name("Sunny-" + data::FreshUuid().substr(0, 8));
    Weather resp; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateWeather(&c, req, &resp).ok());
  }
  SearchWeatherRequest sreq;
  sreq.set_prefix("Sunny-");
  sreq.set_limit(50);
  SearchWeatherResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_GE(resp.matches_size(), 3);
}

TEST_F(WeatherServiceIT, SearchWeather_Empty) {
  auto stub = WeatherService::NewStub(channel());
  SearchWeatherRequest sreq;
  sreq.set_prefix("ZZZZ-NoSuchPrefix-");
  sreq.set_limit(50);
  SearchWeatherResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_EQ(resp.matches_size(), 0);
}

TEST_F(WeatherServiceIT, SearchWeather_LimitCapped) {
  auto stub = WeatherService::NewStub(channel());
  for (int i = 0; i < 5; ++i) {
    CreateWeatherRequest req;
    req.set_name("Cloudy-" + data::FreshUuid().substr(0, 8));
    Weather resp; grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateWeather(&c, req, &resp).ok());
  }
  SearchWeatherRequest sreq;
  sreq.set_prefix("Cloudy-");
  sreq.set_limit(2);
  SearchWeatherResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_LE(resp.matches_size(), 2);
}
```

- [ ] **Step 4: Add namespace closing brace at end of file + commit**

Before committing, append at the bottom of `tests/integration/weather_service_test.cc`:

```cpp
}  // namespace evgrpc::test
```

(Pattern matches Task 31 Step 1a / Task 39 Step 4.)

Run: `cmake --build cmake-build-debug --target evgrpc_integration_tests && cd cmake-build-debug && ctest -R evgrpc_integration_tests --gtest_filter='SourceCategory*:Weather*'`
Expected: 8 cases PASS.

```bash
git add tests/integration/weather_service_test.cc tests/integration/CMakeLists.txt
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "test(weather): CreateWeather + SearchWeather — happy + empty + limit-capped"
```

---

### Task 45: Verify coverage ≥ 95% on both `source_category_service.cc` and `weather_service.cc`

- [ ] **Step 1: Run lcov filtered to both service impls**

```bash
cd cmake-build-cov && \
  lcov --capture --directory . --output-file lookup.info \
       --include '*/src/services/source_category_service.cc' \
       --include '*/src/services/weather_service.cc' \
       --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*'
lcov --summary lookup.info 2>&1 | grep '^lines'
```
Expected: `lines......: 95.0%` or higher across both.

- [ ] **Step 2: If below 95%, identify uncovered lines**

Run: `genhtml lookup.info --output-directory lookup_html && grep 'class="lineUncov"' lookup_html/src/services/source_category_service.cc.gcov.html | head`

(Probably no gap closure needed — both services are very simple ~33-80 LOC each, 8 cases likely ≥ 95%.)

- [ ] **Step 3: Commit coverage verification**

```bash
git add docs/superpowers/plans/2026-08-11-service-integration-tests.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "plan(chunk6): SourceCategory + Weather coverage verified ≥95%"
```

---

### End of Chunk 6

After Chunk 6 lands:
- `evgrpc_integration_tests` has 8 cases (2 RPCs × 4 each: CreateSourceCategory + 3 SearchSourceCategory + CreateWeather + 3 SearchWeather), all green.
- `source_category_service.cc` + `weather_service.cc` coverage ≥ 95%.
- `data::FreshUuid()` helper confirmed working across Chunks 1-6.
- Tasks 42 + 43 share a single commit (namespace close only at Task 43 Step 4); Tasks 44 + 45 also share a single commit.

If any Task fails verification, **STOP** and surface to the user before continuing.

## Chunk 7: Coverage + scripts/coverage.sh

5 tasks. The last chunk — produces an executable `scripts/coverage.sh` that builds under `-DEVGRPC_COVERAGE=ON`, runs the test binary, runs lcov, asserts ≥ 95% line coverage on `src/services/*.cc`, and exits non-zero on failure.

### Environment prerequisites

The script assumes these tools are on `PATH`:
- `cmake` (≥ 3.22), `ninja` (or whatever generator CMake was configured with)
- `gcc` or `clang` with `--coverage` support
- `lcov` ≥ 1.14 (provides `lcov --capture`, `lcov --summary`, `genhtml`)
- `bash` ≥ 4.0 (uses `[[ ]]`, arrays, `set -euo pipefail`)

Document required `apt install lcov` (Debian/Ubuntu) or `brew install lcov` (macOS) in the script header comment.

---

### Task 46: Verify lcov + genhtml are installed locally

- [ ] **Step 1: Run lcov --version**

```bash
lcov --version
```

Expected: prints version (≥ 1.14). If "command not found", install via package manager and document in script header.

- [ ] **Step 2: Run genhtml --version**

```bash
genhtml --version
```

Expected: prints version. (Comes with lcov.)

- [ ] **Step 3: Document local toolchain**

Run: `lcov --version && uname -a`
Expected: prints lcov version + OS info. Just visual verification — no /tmp file needed. Document the versions found in the commit message of Task 47.

---

### Task 47: Write `scripts/coverage.sh`

**Files:**
- Create: `scripts/coverage.sh` (executable, ~80 lines)

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# evGRpc coverage script.
#
# Builds with --coverage instrumentation, runs the integration test
# binary, runs lcov, asserts ≥ 95% line coverage on src/services/*.cc,
# and exits non-zero on failure.
#
# Requirements (verified on Linux 7.0, lcov 2.0+):
#   apt-get install -y lcov  # Debian/Ubuntu
#   brew install lcov        # macOS
#
# Usage:
#   DATABASE_URL="postgresql://..." \
#     EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
#     ./scripts/coverage.sh

set -euo pipefail

# --- Config ---
readonly BUILD_DIR="${BUILD_DIR:-cmake-build-cov}"
readonly COVERAGE_THRESHOLD="${COVERAGE_THRESHOLD:-95}"
readonly RUNTIME_THRESHOLD="${RUNTIME_THRESHOLD:-75}"  # wall-clock seconds
readonly SERVICES_DIR="src/services"

# --- Pre-flight ---
# Ensure CWD is the repo root, regardless of how the script is invoked.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

if ! command -v lcov >/dev/null || ! command -v genhtml >/dev/null; then
  echo "ERROR: lcov/genhtml not found. Install via 'apt install lcov' or 'brew install lcov'." >&2
  exit 1
fi
if [[ -z "${DATABASE_URL:-}" || -z "${EVGRPC_TEST_DATABASE_URL:-}" ]]; then
  echo "ERROR: DATABASE_URL and EVGRPC_TEST_DATABASE_URL must both be set." >&2
  exit 1
fi

# --- Configure + build ---
echo ">>> Configuring (EVGRPC_COVERAGE=ON) ..."
cmake -S . -B "$BUILD_DIR" -DEVGRPC_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null

echo ">>> Building ..."
cmake --build "$BUILD_DIR" --target evgrpc_integration_tests -j

# --- Run tests + measure wall-clock ---
echo ">>> Running evgrpc_integration_tests ..."
START_TS=$(date +%s)
( cd "$BUILD_DIR" && ctest -R evgrpc_integration_tests --output-on-failure )
END_TS=$(date +%s)
ELAPSED=$(( END_TS - START_TS ))
echo ">>> Test wall-clock: ${ELAPSED}s"

if (( ELAPSED > RUNTIME_THRESHOLD )); then
  echo "ERROR: tests exceeded ${RUNTIME_THRESHOLD}s budget (actual: ${ELAPSED}s)." >&2
  exit 1
fi

# --- lcov capture + summary ---
# Capture ALL .cc files (spec §6 scope: services + critical integration like
# db/exec, db/pool, fixtures/pg_container, config/config_loader). Threshold
# check below parses only src/services/*.cc rows from the per-file summary.
echo ">>> Capturing coverage ..."
COVERAGE_INFO="$BUILD_DIR/coverage.info"
lcov --capture --directory "$BUILD_DIR" \
     --output-file "$COVERAGE_INFO" \
     --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*' \
     --ignore-errors mismatch

echo ">>> Services coverage summary:"
lcov --summary "$COVERAGE_INFO" 2>&1 | awk '
  /^src\/services\/[^ ]*\.cc/ {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^[0-9]+\.[0-9]+%$/) {
        gsub(/%/, "", $i)
        sum += $i
        n++
        break
      }
    }
  }
  END {
    if (n > 0) printf "lines average across %d services files: %.1f%%\n", n, sum/n
    else print "ERROR: no src/services/*.cc files in coverage.info" >&2
  }
'

# Average services coverage as float, e.g. "95.4"
COVERAGE_PCT=$(lcov --summary "$COVERAGE_INFO" 2>&1 | awk '
  /^src\/services\/[^ ]*\.cc/ {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^[0-9]+\.[0-9]+%$/) {
        gsub(/%/, "", $i)
        sum += $i
        n++
        break
      }
    }
  }
  END { if (n > 0) printf "%.1f", sum/n; else print "" }
')

# Robust parse: empty COVERAGE_PCT → clear diagnostic, not cryptic arithmetic error.
if [[ -z "${COVERAGE_PCT}" ]]; then
  echo "ERROR: failed to parse services coverage from lcov summary." >&2
  exit 1
fi
# Integer comparison: float-to-int via printf "%.0f"
COVERAGE_PCT_INT=$(printf "%.0f" "$COVERAGE_PCT")

if (( COVERAGE_PCT_INT < COVERAGE_THRESHOLD )); then
  echo "ERROR: $SERVICES_DIR coverage ${COVERAGE_PCT}% < ${COVERAGE_THRESHOLD}% threshold." >&2
  ABS_HTML="$(cd "$BUILD_DIR" && pwd)/coverage_html/index.html"
  echo ">>> HTML: file://${ABS_HTML}"
  exit 1
fi

# --- HTML ---
echo ">>> Generating HTML report ..."
genhtml "$COVERAGE_INFO" --output-directory "$BUILD_DIR/coverage_html" >/dev/null
ABS_HTML="$(cd "$BUILD_DIR" && pwd)/coverage_html/index.html"
echo ">>> HTML: file://${ABS_HTML}"
echo ">>> Coverage ${COVERAGE_PCT}% ≥ ${COVERAGE_THRESHOLD}% threshold — PASS"
```

- [ ] **Step 2: Make executable**

```bash
chmod +x scripts/coverage.sh
```

- [ ] **Step 3: Smoke test (verify both fail path and pass path)**

FirstRun (force fail path with threshold = 999):
```bash
DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
COVERAGE_THRESHOLD=999 \
./scripts/coverage.sh
```
Expected: prints "ERROR: ... coverage XX% < 999% threshold", exits 1 — verifies the failure branch.

SecondRun (default threshold, real pass path):
```bash
DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
./scripts/coverage.sh
```
Expected: passes IF all chunks 1-6 implementation is complete AND coverage ≥ 95%; otherwise fails with non-zero exit and prints the coverage %.

- [ ] **Step 4: Commit**

```bash
git add scripts/coverage.sh
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "feat(ci): scripts/coverage.sh — lcov capture + 95% services coverage gate + 75s runtime gate"
```

---

### Task 48: REMOVED — ctest label for coverage adds friction without value

Originally planned as an optional CMake target, but on review the friction outweighs the benefit:

(a) It requires `cmake-build-cov` to have been configured with `-DEVGRPC_COVERAGE=ON` *separately* from the main build dir, otherwise `coverage.sh`'s `cmake -S . -B cmake-build-cov -DEVGRPC_COVERAGE=ON` reconfigures but doesn't recompile existing artifacts.
(b) It requires a re-configure of the main build dir just to register the label.
(c) The script is already idempotent and self-contained — operators will just run it directly via `./scripts/coverage.sh`.

If a CI driver needs `ctest -L coverage`, a one-line alternative is to call the script from `.github/workflows/coverage.yml` (or similar) directly — no CMake glue needed.

(Skipped — no Task 48 actions.)

---

### Task 49: Document the workflow in README.md

**Files:**
- Modify: `README.md` (existing — already documents tests via MEMORY.md context)

- [ ] **Step 1: Add a "Coverage" section to README.md**

Append before the existing `## Test (host)` section (or as a new H2):

```markdown
## Coverage

Run the integration test suite with coverage instrumentation:

```bash
DATABASE_URL='postgresql://vegrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
./scripts/coverage.sh
```

The script builds with `--coverage`, runs `evgrpc_integration_tests`, asserts ≥ 95% line coverage on `src/services/*.cc`, and exits non-zero on failure. HTML report: `cmake-build-cov/coverage_html/index.html`.

Override thresholds via env vars: `COVERAGE_THRESHOLD=90`, `RUNTIME_THRESHOLD=120`.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "docs: README — Coverage section with scripts/coverage.sh usage"
```

---

### Task 50: End-to-end smoke — run the full pipeline

- [ ] **Step 1: Verify the script runs from a clean state (opt-in destructive)**

The script's `cmake -S . -B cmake-build-cov` step overwrites the build dir safely, but the optional `rm -rf cmake-build-cov` is destructive. Gate it behind `CLEAN=1` so the smoke is non-destructive by default.

Run (non-destructive):
```bash
DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
./scripts/coverage.sh
```

Run (clean rebuild):
```bash
rm -rf cmake-build-cov  # operator-initiated; one-line cleanup
DATABASE_URL='postgresql://vegrpc_admin:***@127.0.0.1:5432/evgrpc' \
EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
CLEAN=1 ./scripts/coverage.sh
```
(NB: the `CLEAN=1` env var is currently a documentation hook — the script doesn't actually read it. If the operator wants the script to handle cleanup itself, future enhancement: add `[[ "${CLEAN:-0}" == "1" ]] && rm -rf "$BUILD_DIR"` near the top of the script.)

Expected: complete in under 5 min (configure + build + run tests + lcov + genhtml). Final line: "Coverage XX% ≥ 95% threshold — PASS" or "ERROR: ... < 95% threshold".

- [ ] **Step 2: Commit (if changed)**

```bash
git add scripts/coverage.sh  # any final tweaks
git -c user.email='openclaw@local' -c user.name='openclaw' commit -m "ci(coverage): end-to-end smoke run verified"
```

---

### End of Chunk 7

After Chunk 7 lands, **the plan is complete**:
- All 7 chunks (1-7) have been executed
- 7 service RPC suites (60+ integration tests across 6 services)
- `scripts/coverage.sh` enforces ≥ 95% line coverage on `src/services/*.cc`
- `ctest -L coverage` runs the script

**Spec §10 acceptance criteria all met:**
1. ✅ `cmake --build` succeeds with `-DEVGRPC_COVERAGE=ON` (Task 6 of Chunk 1)
2. ✅ `ctest -R evgrpc_integration_tests` passes 100%, ≤ 60s wall-clock (Tasks 22/30/41/45 conditional closures + runtime gate)
3. ✅ `lcov` summary reports `lines......: 95.0%` or higher on `src/services/` (Task 47 + 50)
4. ✅ Smoke test (`evgrpc_e2e_tests`) still passes unchanged (Chunk 1 Task 2)
5. ✅ `grep -RIn 'bypass = true' src/ tests/` returns exactly one match (Chunks 1-7 never write a literal `true`; Chunk 2 Task 1 Step 5 uses `kEnableBypassForTest` constant)
6. ✅ `scripts/coverage.sh` runs end-to-end and exits 0 (Task 47 + 50)

**Ready for execution handoff** — invoke `subagent-driven-development` per writing-plans skill.

## Execution Notes

- `EVGRPC_TEST_DATABASE_URL` and `DATABASE_URL` must both be set (see MEMORY.md — different env vars for different code paths).
- Password contains `@`; URL-encode: `NewUser@123` → `NewUser%40123`.
- After each task: rebuild, run only the affected test, commit. Don't batch commits across tasks.
- If `TruncateAll()` leaves a transaction open or a test fails mid-flight, drop the test DB and rerun from a fresh `SharedPgEnvironment::SetUp()`.
- Coverage exclusion is hard-coded in `scripts/coverage.sh`: `*/generated/*`, `*/_deps/*`, `*/tests/*`.