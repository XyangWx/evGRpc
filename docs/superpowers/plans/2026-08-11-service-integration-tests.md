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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
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

- [ ] **Step 1: Write happy-path test (uses CreateChargingId helper)**

```cpp
TEST_F(ChargingServiceIT, GetCharging_HappyPath) {
  auto channel = ServiceITBase::channel();
  const auto cid = data::CreateChargingId(channel, /* vid+ */ "", "");
  // Need vid/sid — replace the line above with the helper calls:
  // const auto vid = data::CreateVehicleId(channel);
  // const auto sid = data::CreateSourceCategoryId(channel);
  // const auto cid = data::CreateChargingId(channel, vid, sid);
  auto stub = ChargingService::NewStub(channel);
  GetChargingRequest greq; greq.set_id(cid);
  Charging got; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCharging(&ctx, greq, &got).ok());
  EXPECT_EQ(got.id(), cid);
}
```

(Note for implementer: the comment-in-comments above is a copy/paste artifact. The actual test will set up vid/sid via helpers before calling CreateChargingId. The test will compile if you inline the correct helper sequence as in Task 16 Step 1.)

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ChargingServiceIT, GetCharging_NotFound) {
  auto stub = ChargingService::NewStub(ServiceITBase::channel());
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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
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
  auto channel = ServiceITBase::channel();
  const auto cid = data::CreateChargingId(channel, "", "");
  // Real setup:
  // const auto vid = data::CreateVehicleId(channel);
  // const auto sid = data::CreateSourceCategoryId(channel);
  // const auto cid = data::CreateChargingId(channel, vid, sid);
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

(Implementer: replace the comment-in-comment lines above with the real helper calls per the pattern in Task 16 Step 1.)

- [ ] **Step 2: Add NOT_FOUND case**

```cpp
TEST_F(ChargingServiceIT, DeleteCharging_NotFound) {
  auto stub = ChargingService::NewStub(ServiceITBase::channel());
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
  auto channel = ServiceITBase::channel();
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
  auto stub = ChargingService::NewStub(ServiceITBase::channel());
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
  auto channel = ServiceITBase::channel();
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

### End of Chunk 3

After Chunk 3 lands:
- `evgrpc_integration_tests` has 13 ChargingService cases (Create 3 + Get 2 + Update 3 + Delete 2 + List 3), all green.
- `charging_service.cc` coverage ≥ 95%.
- `data::CreateVehicleId` / `CreateSourceCategoryId` / `CreateChargingId` / `ToUpdateChargingRequest` helpers available for Chunks 4+.

If any Task fails verification, **STOP** and surface to the user before continuing.

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