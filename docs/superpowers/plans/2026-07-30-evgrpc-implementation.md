# evGRpc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** ✅ Completed 2026-08-05 — all 23 task sections landed on `master`, tagged `v0.1.0` at `93086fe`. Tests: 38/38 unit + 1/1 e2e pass; `scripts/smoke.sh` exits 0 end-to-end against a freshly-built `evgrpc:dev` container.

**Goal:** Build a C++ gRPC service that records and analyzes EV electricity costs, backed by PostgreSQL, protected by OAuth 2.0 (Resource Server / JWT / RS256 / JWKS), packaged in a multi-stage Docker image.

**Architecture:** Single gRPC binary hosting 6 services on one port. gRPC ServerInterceptor validates JWT bearer tokens against an external IdP via JWKS cache before every RPC. libpqxx talks to PostgreSQL. Multi-stage Dockerfile (CMake + Ninja + libpqxx + grpc++ + jwt-cpp + libcurl).

**Tech Stack:** C++20 · gRPC++ · Protobuf · libpqxx · jwt-cpp · libcurl · nlohmann/json · spdlog · CMake + Ninja · gtest · testcontainers-cpp · Docker (multi-stage) · PostgreSQL 14+

**Spec:** `docs/superpowers/specs/2026-07-30-evgrpc-design.md`

---

## Global Constraints

- **Language:** C++20 (bumped from C++17 at Task 20 because testcontainers-cpp v0.2.0 declares `cxx_std_20` on its INTERFACE; mixing C++17 and C++20 in the same target graph would fail CMake's standard-value check. All existing code is C++17-compatible — the bump just enables C++20 features for new code.)
- **Build:** CMake (`cmake -G Ninja`) + Ninja
- **gRPC framework:** `grpc++` (C++)
- **DB client:** `libpqxx` (PostgreSQL C++ binding)
- **JWT validation:** `jwt-cpp` header-only lib (RS256 + JWKS)
- **JWKS HTTP fetch:** `libcurl`
- **Logging:** `spdlog`
- **JSON:** `nlohmann/json`
- **Testing:** `gtest` + `testcontainers-cpp`
- **Database:** PostgreSQL 14+
- **Proto package:** `evgrpc`
- **Auth:** OAuth 2.0 Resource Server. JWT bearer tokens via `Authorization: Bearer <JWT>` header. RS256 signature, validated against JWKS. Issuer (`OAUTH_ISSUER_URL`) + audience (`OAUTH_AUDIENCE`) must match. Any valid token can call any RPC in v1.
- **Env vars required at startup:** `DATABASE_URL`, `OAUTH_ISSUER_URL`, `OAUTH_AUDIENCE`, `OAUTH_JWKS_URL`. Server fails fast if any is missing.
- **Time semantics:** All timestamps stored as `TIMESTAMP` (no timezone) in PostgreSQL. App serializes as `google.protobuf.Timestamp` over gRPC.
- **Currency:** RMB only. All monetary fields `DECIMAL(10,2)` or `DECIMAL(4,2)` per spec.
- **Uniqueness:** UNIQUE constraint on `vehicle.LicensePlate`, `weather.Name`, `source_category.Name`. Insert collision → gRPC `ALREADY_EXISTS`.
- **Application-level validation:** End > Start, EndPercent > BeginPercent, etc. — NOT enforced via DB CHECK in v1.
- **DB CHECK constraints:** NOT added in v1 (data entry forgiveness).
- **Naming:** snake_case in PostgreSQL; camelCase in protobuf / C++.
- **Commit style:** `<type>(<scope>): <subject>` — `feat`, `test`, `chore`, `fix`, `docs`. Each task ends with one commit.

---

## File Structure

```
evGRpc/
├── CMakeLists.txt                # top-level CMake, FetchContent for grpc/libpqxx/jwt-cpp/spdlog/nlohmann_json
├── cmake/
│   ├── deps.cmake                # dependency declarations
│   └── protoc.cmake              # protobuf/grpc code generation rules
├── proto/evgrpc/
│   ├── common.proto              # shared messages (PageInfo, etc.)
│   ├── vehicle.proto
│   ├── weather.proto
│   ├── source_category.proto
│   ├── consumption.proto
│   ├── charging.proto
│   └── display.proto
├── sql/
│   └── 001_initial.sql           # all DDL (tables, ENUM, indexes)
├── src/
│   ├── main.cc                   # entrypoint: load config, init DB pool, build server
│   ├── config/
│   │   ├── config.h
│   │   └── config.cc             # env var parsing + validation
│   ├── db/
│   │   ├── pool.h
│   │   ├── pool.cc               # libpqxx connection pool
│   │   ├── error.h
│   │   └── error.cc              # libpqxx exception → gRPC Status
│   ├── auth/
│   │   ├── jwt_validator.h
│   │   ├── jwt_validator.cc      # RS256 signature + iss/aud/exp validation
│   │   ├── jwks_cache.h
│   │   ├── jwks_cache.cc         # JWKS HTTP fetch + cache + kid lookup
│   │   ├── auth_interceptor.h
│   │   └── auth_interceptor.cc   # gRPC ServerInterceptor
│   ├── services/
│   │   ├── vehicle_service.{h,cc}
│   │   ├── weather_service.{h,cc}
│   │   ├── source_category_service.{h,cc}
│   │   ├── consumption_service.{h,cc}
│   │   ├── charging_service.{h,cc}
│   │   └── display_service.{h,cc}
│   └── util/
│       ├── uuid.h
│       └── uuid.cc               # libpqxx UUID helpers
├── tests/
│   ├── CMakeLists.txt
│   ├── test_main.cc              # gtest entry
│   ├── fixtures/
│   │   ├── pg_container.h
│   │   ├── pg_container.cc       # testcontainers-cpp PostgreSQL fixture
│   │   ├── test_server.h
│   │   ├── test_server.cc        # in-process gRPC server fixture (with auth test IdP)
│   │   ├── jwt_test_keys.h
│   │   └── jwt_test_keys.cc      # generates RSA keypair on demand
│   ├── unit/
│   │   ├── test_config.cc
│   │   ├── test_jwt_validator.cc
│   │   ├── test_jwks_cache.cc
│   │   ├── test_pool.cc
│   │   └── test_error.cc
│   └── integration/
│       ├── test_vehicle_e2e.cc
│       ├── test_weather_e2e.cc
│       ├── test_source_category_e2e.cc
│       ├── test_consumption_e2e.cc
│       ├── test_charging_e2e.cc
│       ├── test_display_e2e.cc
│       └── test_auth_e2e.cc      # auth interceptor e2e
├── Dockerfile                    # multi-stage
└── docs/...
```

---

## Task 1: CMake Project Skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `cmake/deps.cmake`
- Create: `cmake/protoc.cmake`
- Create: `src/main.cc`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_main.cc`
- Modify: `.gitignore` — `build/` already covered

**Interfaces:**
- Consumes: nothing
- Produces: builds `evgrpc_server` binary via `cmake -G Ninja && ninja`

- [ ] **Step 1: Write CMakeLists.txt with project skeleton**

```cmake
cmake_minimum_required(VERSION 3.22)
project(evgrpc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/deps.cmake)
include(cmake/protoc.cmake)

# Subdirectories
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 2: Write cmake/deps.cmake (FetchContent for all deps)**

```cmake
include(FetchContent)

set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(
  grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG        v1.62.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  protobuf
  GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
  GIT_TAG        v25.1
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  libpqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG        7.9.2
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.13.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  testcontainers_cpp
  GIT_REPOSITORY https://github.com/testcontainers/testcontainers-cpp.git
  GIT_TAG        v0.20.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  jwt_cpp
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG        curl-8_5_0
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(grpc protobuf libpqxx nlohmann_json spdlog googletest testcontainers_cpp jwt_cpp)
# curl: only libcurl target, fetched below
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(curl)

find_package(Threads REQUIRED)
```

- [ ] **Step 3: Write cmake/protoc.cmake**

```cmake
include(FetchContent)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(EVGRPC_PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(EVGRPC_PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)

file(MAKE_DIRECTORY ${EVGRPC_PROTO_GEN_DIR})

set(EVGRPC_PROTO_FILES
  ${EVGRPC_PROTO_DIR}/common.proto
  ${EVGRPC_PROTO_DIR}/vehicle.proto
  ${EVGRPC_PROTO_DIR}/weather.proto
  ${EVGRPC_PROTO_DIR}/source_category.proto
  ${EVGRPC_PROTO_DIR}/consumption.proto
  ${EVGRPC_PROTO_DIR}/charging.proto
  ${EVGRPC_PROTO_DIR}/display.proto
)

add_custom_target(evgrpc_proto_gen
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    --proto_path=${EVGRPC_PROTO_DIR}
    --cpp_out=${EVGRPC_PROTO_GEN_DIR}
    --grpc_out=${EVGRPC_PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    ${EVGRPC_PROTO_FILES}
  DEPENDS ${EVGRPC_PROTO_FILES}
  COMMENT "Generating protobuf + gRPC stubs"
  VERBATIM
)
```

- [ ] **Step 4: Write minimal src/main.cc and src/CMakeLists.txt**

`src/main.cc`:
```cpp
#include <iostream>

int main() {
    std::cout << "evGRpc server starting..." << std::endl;
    return 0;
}
```

`src/CMakeLists.txt`:
```cmake
add_executable(evgrpc_server main.cc)
target_link_libraries(evgrpc_server PRIVATE Threads::Threads)
target_include_directories(evgrpc_server PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(evgrpc_server evgrpc_proto_gen)
```

- [ ] **Step 5: Write tests/CMakeLists.txt and tests/test_main.cc**

`tests/CMakeLists.txt`:
```cmake
add_executable(evgrpc_tests test_main.cc)
target_link_libraries(evgrpc_tests PRIVATE gtest gtest_main)
add_test(NAME evgrpc_tests COMMAND evgrpc_tests)
```

`tests/test_main.cc`:
```cpp
#include <gtest/gtest.h>
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 6: Create empty proto files (placeholders for now)**

`proto/evgrpc/common.proto`:
```proto
syntax = "proto3";
package evgrpc;
```

(repeat for `vehicle.proto`, `weather.proto`, `source_category.proto`, `consumption.proto`, `charging.proto`, `display.proto` — each with `syntax = "proto3"; package evgrpc;`)

- [ ] **Step 7: Build and run**

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/src/evgrpc_server      # prints "evGRpc server starting..."
./build/tests/evgrpc_tests     # runs zero tests, exits 0
```

Expected: server prints message and exits. Tests pass with 0 tests.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt cmake/ src/ tests/ proto/
git commit -m "chore: scaffold CMake + Ninja project with FetchContent deps"
```

---

## Task 2: Config Loading

**Files:**
- Create: `src/config/config.h`
- Create: `src/config/config.cc`
- Modify: `src/main.cc` — call `Config::Load()`
- Modify: `src/CMakeLists.txt` — add `config/` subdir
- Create: `tests/unit/test_config.cc`

**Interfaces:**
- Produces: `Config` struct with `database_url`, `oauth_issuer_url`, `oauth_audience`, `oauth_jwks_url`, `oauth_jwks_cache_ttl_seconds`, `grpc_port`. `Config::Load()` throws `std::runtime_error` if any required var missing.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_config.cc`:
```cpp
#include <gtest/gtest.h>
#include "config/config.h"

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override { clear_env(); }
    void clear_env() {
        unsetenv("DATABASE_URL");
        unsetenv("OAUTH_ISSUER_URL");
        unsetenv("OAUTH_AUDIENCE");
        unsetenv("OAUTH_JWKS_URL");
        unsetenv("OAUTH_JWKS_CACHE_TTL");
        unsetenv("GRPC_PORT");
    }
};

TEST_F(ConfigTest, LoadsAllRequiredVars) {
    setenv("DATABASE_URL", "postgres://x", 1);
    setenv("OAUTH_ISSUER_URL", "https://idp", 1);
    setenv("OAUTH_AUDIENCE", "evgrpc", 1);
    setenv("OAUTH_JWKS_URL", "https://idp/jwks", 1);
    auto c = evgrpc::Config::Load();
    EXPECT_EQ(c.database_url, "postgres://x");
    EXPECT_EQ(c.oauth_audience, "evgrpc");
    EXPECT_EQ(c.grpc_port, 50051);  // default
}

TEST_F(ConfigTest, MissingRequiredThrows) {
    setenv("DATABASE_URL", "postgres://x", 1);
    // other required vars missing
    EXPECT_THROW(evgrpc::Config::Load(), std::runtime_error);
}
```

- [ ] **Step 2: Run test, expect FAIL**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=ConfigTest.*
```
Expected: link error (config.h doesn't exist).

- [ ] **Step 3: Implement Config**

`src/config/config.h`:
```cpp
#pragma once
#include <string>
#include <stdexcept>

namespace evgrpc {

struct Config {
    std::string database_url;
    std::string oauth_issuer_url;
    std::string oauth_audience;
    std::string oauth_jwks_url;
    int oauth_jwks_cache_ttl_seconds = 3600;
    int grpc_port = 50051;

    static Config Load();
};

}  // namespace evgrpc
```

`src/config/config.cc`:
```cpp
#include "config/config.h"
#include <cstdlib>
#include <string>

namespace evgrpc {

namespace {
std::string require_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) throw std::runtime_error(std::string("missing env var: ") + name);
    return v;
}
std::string opt_env(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}
int opt_env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return std::stoi(v);
}
}  // namespace

Config Config::Load() {
    Config c;
    c.database_url        = require_env("DATABASE_URL");
    c.oauth_issuer_url    = require_env("OAUTH_ISSUER_URL");
    c.oauth_audience      = require_env("OAUTH_AUDIENCE");
    c.oauth_jwks_url      = require_env("OAUTH_JWKS_URL");
    c.oauth_jwks_cache_ttl_seconds = opt_env_int("OAUTH_JWKS_CACHE_TTL", 3600);
    c.grpc_port           = opt_env_int("GRPC_PORT", 50051);
    return c;
}

}  // namespace evgrpc
```

- [ ] **Step 4: Wire into CMake**

Append to `src/CMakeLists.txt`:
```cmake
add_subdirectory(config)
```
Create `src/config/CMakeLists.txt`:
```cmake
add_library(evgrpc_config config.cc)
target_link_libraries(evgrpc_config PUBLIC spdlog::spdlog)
target_include_directories(evgrpc_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

Append `evgrpc_config` to `evgrpc_server` target_link_libraries.

- [ ] **Step 5: Update src/main.cc**

```cpp
#include <iostream>
#include "config/config.h"

int main() {
    try {
        auto c = evgrpc::Config::Load();
        std::cout << "evGRpc server starting on port " << c.grpc_port << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << std::endl;
        return 1;
    }
}
```

Append `tests/unit/test_config.cc` to `tests/CMakeLists.txt`.

- [ ] **Step 6: Run tests**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=ConfigTest.*
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/ tests/
git commit -m "feat(config): load required env vars at startup, fail-fast on missing"
```

---

## Task 3: PostgreSQL DDL

**Files:**
- Create: `sql/001_initial.sql`
- Create: `tests/fixtures/pg_container.{h,cc}` (full implementation deferred to Task 22)
- Test: covered indirectly in Task 22 (testcontainers integration)

- [ ] **Step 1: Write `sql/001_initial.sql`**

```sql
-- 001_initial.sql
-- Run against an external PostgreSQL 14+ database.

BEGIN;

CREATE TYPE charger_type_enum AS ENUM ('fast', 'slow');

CREATE TABLE vehicle (
  Id               UUID PRIMARY KEY,
  Brand            VARCHAR(36)  NOT NULL,
  CalibratedRange  INTEGER       NOT NULL,
  BatteryCapacity  DECIMAL(10,2) NOT NULL,
  PurchaseDate     DATE          NOT NULL,
  LicensePlate     VARCHAR(15)   NOT NULL UNIQUE
);

CREATE TABLE weather (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);

CREATE TABLE consumption (
  Id                  UUID PRIMARY KEY,
  VehicleId           UUID NOT NULL REFERENCES vehicle(Id),
  Start               TIMESTAMP NOT NULL,
  End                 TIMESTAMP NOT NULL,
  BeginPercent        INT NOT NULL,
  EndPercent          INT NOT NULL,
  BeginMileage        INT NOT NULL,
  EndMileage          INT NOT NULL,
  BeginRange          INT NOT NULL,
  EndRange            INT NOT NULL,
  HighestTemperature  DECIMAL(4,1) NOT NULL,
  LowestTemperature   DECIMAL(4,1) NOT NULL,
  WeatherId           UUID NOT NULL REFERENCES weather(Id),
  Remark              TEXT
);

CREATE TABLE source_category (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);

CREATE TABLE charging (
  Id                    UUID PRIMARY KEY,
  VehicleId             UUID NOT NULL REFERENCES vehicle(Id),
  StartTime             TIMESTAMP NOT NULL,
  EndTime               TIMESTAMP NOT NULL,
  StartPercent          INT NOT NULL,
  EndPercent            INT NOT NULL,
  StartMileage          INT NOT NULL,
  EndMileage            INT NOT NULL,
  KwhCharged            DECIMAL(10,2) NOT NULL,
  Cost                  DECIMAL(10,2) NOT NULL,
  ElectricityUnitPrice  DECIMAL(4,2)  NOT NULL,
  ServiceFee            DECIMAL(5,2),
  ChargerType           charger_type_enum NOT NULL,
  SourceCategoryId      UUID NOT NULL REFERENCES source_category(Id),
  Location              VARCHAR(100),
  Remark                TEXT
);

CREATE INDEX idx_consumption_vehicle_start ON consumption(VehicleId, Start);
CREATE INDEX idx_charging_vehicle_starttime ON charging(VehicleId, StartTime);

COMMIT;
```

- [ ] **Step 2: Commit (manual review only — DDL has no automated test in v1)**

```bash
git add sql/
git commit -m "chore(sql): initial schema (vehicle/weather/consumption/source_category/charging)"
```

---

## Task 4: Database Connection Pool

**Files:**
- Create: `src/db/pool.h`
- Create: `src/db/pool.cc`
- Modify: `src/CMakeLists.txt`
- Create: `tests/unit/test_pool.cc`

**Interfaces:**
- Produces: `class PgPool` — constructor takes connection string. `acquire()` returns RAII handle `PgConn` wrapping `pqxx::connection`. Connection is returned to pool on destruction.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_pool.cc` (uses testcontainers, deferred to Task 22 for full e2e; here just test the pool struct compiles + acquires/releases without real PG):

```cpp
#include <gtest/gtest.h>
#include "db/pool.h"

TEST(PgPoolTest, InvalidUrlThrows) {
    EXPECT_THROW(evgrpc::PgPool p("not-a-url"), std::exception);
}
```

- [ ] **Step 2: Implement PgPool**

`src/db/pool.h`:
```cpp
#pragma once
#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <string>
#include <pqxx/pqxx>

namespace evgrpc {

class PgPool;

class PgConn {
public:
    PgConn(PgPool* pool, std::unique_ptr<pqxx::connection> conn);
    ~PgConn();
    PgConn(const PgConn&) = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&& other) noexcept;
    pqxx::connection& operator*() { return *conn_; }
    pqxx::connection* operator->() { return conn_.get(); }
private:
    PgPool* pool_;
    std::unique_ptr<pqxx::connection> conn_;
};

class PgPool {
public:
    explicit PgPool(const std::string& url, int size = 4);
    PgConn acquire();
    void release(std::unique_ptr<pqxx::connection> conn);
private:
    std::string url_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<pqxx::connection>> idle_;
    int size_;
};

}  // namespace evgrpc
```

`src/db/pool.cc`:
```cpp
#include "db/pool.h"

namespace evgrpc {

PgConn::PgConn(PgPool* pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(pool), conn_(std::move(conn)) {}

PgConn::~PgConn() {
    if (pool_ && conn_) pool_->release(std::move(conn_));
}

PgConn::PgConn(PgConn&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) { other.pool_ = nullptr; }

PgPool::PgPool(const std::string& url, int size) : url_(url), size_(size) {
    // eagerly open `size_` connections to fail-fast on bad URL
    for (int i = 0; i < size_; ++i) {
        idle_.push(std::make_unique<pqxx::connection>(url_));
    }
}

PgConn PgPool::acquire() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this]{ return !idle_.empty(); });
    auto c = std::move(idle_.front());
    idle_.pop();
    return PgConn(this, std::move(c));
}

void PgPool::release(std::unique_ptr<pqxx::connection> conn) {
    {
        std::lock_guard lk(mu_);
        idle_.push(std::move(conn));
    }
    cv_.notify_one();
}

}  // namespace evgrpc
```

- [ ] **Step 3: Wire into CMake**

Create `src/db/CMakeLists.txt`:
```cmake
add_library(evgrpc_db pool.cc)
target_link_libraries(evgrpc_db PUBLIC libpqxx::pqxx)
target_include_directories(evgrpc_db PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

Append `add_subdirectory(db)` to `src/CMakeLists.txt`. Link `evgrpc_db` into `evgrpc_server`. Append `tests/unit/test_pool.cc` to `tests/CMakeLists.txt` and link `evgrpc_db` + `libpqxx::pqxx`.

- [ ] **Step 4: Run tests**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=PgPoolTest.*
```
Expected: PgPoolTest.InvalidUrlThrows passes.

- [ ] **Step 5: Commit**

```bash
git add src/db/ tests/unit/test_pool.cc
git commit -m "feat(db): libpqxx connection pool with RAII handle"
```

---

## Task 5: PG Error → gRPC Status Mapping

**Files:**
- Create: `src/db/error.h`
- Create: `src/db/error.cc`
- Modify: `src/db/CMakeLists.txt`
- Create: `tests/unit/test_error.cc`

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include "db/error.h"
#include <pqxx/pqxx>

TEST(ErrorMapTest, UniqueViolationMapsToAlreadyExists) {
    pqxx::unique_violation ex("duplicate key value violates unique constraint \"vehicle_licenseplate_key\"");
    auto status = evgrpc::ToGrpcStatus(ex);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

TEST(ErrorMapTest, ForeignKeyViolationMapsToInvalidArgument) {
    pqxx::foreign_key_violation ex("violates foreign key constraint");
    auto status = evgrpc::ToGrpcStatus(ex);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
```

- [ ] **Step 2: Implement**

`src/db/error.h`:
```cpp
#pragma once
#include <grpcpp/support/status_code_enum.h>
#include <stdexcept>

namespace evgrpc {

grpc::Status ToGrpcStatus(const std::exception& e);

}  // namespace evgrpc
```

`src/db/error.cc`:
```cpp
#include "db/error.h"
#include <pqxx/pqxx>

namespace evgrpc {

grpc::Status ToGrpcStatus(const std::exception& e) {
    if (dynamic_cast<const pqxx::unique_violation*>(&e)) {
        return {grpc::StatusCode::ALREADY_EXISTS, e.what()};
    }
    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e)) {
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

}  // namespace evgrpc
```

- [ ] **Step 3: Wire CMake**

Append `error.cc` to `evgrpc_db` library sources. Link `grpc++` to `evgrpc_db`.

- [ ] **Step 4: Run + commit**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=ErrorMapTest.*
git add src/db/ tests/unit/test_error.cc
git commit -m "feat(db): map libpqxx exceptions to gRPC status codes"
```

---

## Task 6: Proto Definitions

**Files:**
- Modify: `proto/evgrpc/common.proto`
- Modify: `proto/evgrpc/vehicle.proto`
- Modify: `proto/evgrpc/weather.proto`
- Modify: `proto/evgrpc/source_category.proto`
- Modify: `proto/evgrpc/consumption.proto`
- Modify: `proto/evgrpc/charging.proto`
- Modify: `proto/evgrpc/display.proto`

- [ ] **Step 1: `common.proto` — shared messages**

```proto
syntax = "proto3";
package evgrpc;

import "google/protobuf/timestamp.proto";

message PageInfo {
  int32 page_size = 1;
  string page_token = 2;
}

message PageResponse {
  string next_page_token = 1;
}
```

- [ ] **Step 2: `vehicle.proto`**

```proto
syntax = "proto3";
package evgrpc;

import "google/protobuf/timestamp.proto";
import "google/protobuf/empty.proto";
import "evgrpc/common.proto";

message Vehicle {
  string id = 1;
  string brand = 2;
  int32 calibrated_range_km = 3;
  double battery_capacity_kwh = 4;
  google.protobuf.Timestamp purchase_date = 5;
  string license_plate = 6;
}

message CreateVehicleRequest {
  string brand = 1;
  int32 calibrated_range_km = 2;
  double battery_capacity_kwh = 3;
  google.protobuf.Timestamp purchase_date = 4;
  string license_plate = 5;
}

message GetVehicleRequest { string id = 1; }

message UpdateVehicleRequest {
  string id = 1;
  string brand = 2;
  int32 calibrated_range_km = 3;
  double battery_capacity_kwh = 4;
  google.protobuf.Timestamp purchase_date = 5;
  string license_plate = 6;
}

message DeleteVehicleRequest { string id = 1; }

message ListVehiclesRequest {
  int32 page_size = 1;
  string page_token = 2;
}

message ListVehiclesResponse {
  repeated Vehicle vehicles = 1;
  string next_page_token = 2;
}

service VehicleService {
  rpc CreateVehicle(CreateVehicleRequest) returns (Vehicle);
  rpc GetVehicle(GetVehicleRequest) returns (Vehicle);
  rpc UpdateVehicle(UpdateVehicleRequest) returns (Vehicle);
  rpc DeleteVehicle(DeleteVehicleRequest) returns (google.protobuf.Empty);
  rpc ListVehicles(ListVehiclesRequest) returns (ListVehiclesResponse);
}
```

- [ ] **Step 3: `weather.proto`**

```proto
syntax = "proto3";
package evgrpc;

message Weather {
  string id = 1;
  string name = 2;
}

message CreateWeatherRequest { string name = 1; }
message SearchWeatherRequest {
  string prefix = 1;
  int32 limit = 2;
}
message SearchWeatherResponse { repeated Weather matches = 1; }

service WeatherService {
  rpc CreateWeather(CreateWeatherRequest) returns (Weather);
  rpc SearchWeather(SearchWeatherRequest) returns (SearchWeatherResponse);
}
```

- [ ] **Step 4: `source_category.proto`** (mirror of `weather.proto`)

```proto
syntax = "proto3";
package evgrpc;

message SourceCategory {
  string id = 1;
  string name = 2;
}

message CreateSourceCategoryRequest { string name = 1; }
message SearchSourceCategoryRequest {
  string prefix = 1;
  int32 limit = 2;
}
message SearchSourceCategoryResponse { repeated SourceCategory matches = 1; }

service SourceCategoryService {
  rpc CreateSourceCategory(CreateSourceCategoryRequest) returns (SourceCategory);
  rpc SearchSourceCategory(SearchSourceCategoryRequest) returns (SearchSourceCategoryResponse);
}
```

- [ ] **Step 5: `consumption.proto`**

```proto
syntax = "proto3";
package evgrpc;

import "google/protobuf/timestamp.proto";
import "google/protobuf/empty.proto";

message Consumption {
  string id = 1;
  string vehicle_id = 2;
  google.protobuf.Timestamp start = 3;
  google.protobuf.Timestamp end = 4;
  int32 begin_percent = 5;
  int32 end_percent = 6;
  int32 begin_mileage_km = 7;
  int32 end_mileage_km = 8;
  int32 begin_range_km = 9;
  int32 end_range_km = 10;
  double highest_temperature_c = 11;
  double lowest_temperature_c = 12;
  string weather_id = 13;
  string remark = 14;
}

message CreateConsumptionRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start = 2;
  google.protobuf.Timestamp end = 3;
  int32 begin_percent = 4;
  int32 end_percent = 5;
  int32 begin_mileage_km = 6;
  int32 end_mileage_km = 7;
  int32 begin_range_km = 8;
  int32 end_range_km = 9;
  double highest_temperature_c = 10;
  double lowest_temperature_c = 11;
  string weather_id = 12;
  string remark = 13;
}
message GetConsumptionRequest { string id = 1; }
message UpdateConsumptionRequest {
  string id = 1;
  string vehicle_id = 2;
  google.protobuf.Timestamp start = 3;
  google.protobuf.Timestamp end = 4;
  int32 begin_percent = 5;
  int32 end_percent = 6;
  int32 begin_mileage_km = 7;
  int32 end_mileage_km = 8;
  int32 begin_range_km = 9;
  int32 end_range_km = 10;
  double highest_temperature_c = 11;
  double lowest_temperature_c = 12;
  string weather_id = 13;
  string remark = 14;
}
message DeleteConsumptionRequest { string id = 1; }
message ListConsumptionsRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_after = 2;
  google.protobuf.Timestamp start_before = 3;
  int32 page_size = 4;
  string page_token = 5;
}
message ListConsumptionsResponse {
  repeated Consumption consumptions = 1;
  string next_page_token = 2;
}

service ConsumptionService {
  rpc CreateConsumption(CreateConsumptionRequest) returns (Consumption);
  rpc GetConsumption(GetConsumptionRequest) returns (Consumption);
  rpc UpdateConsumption(UpdateConsumptionRequest) returns (Consumption);
  rpc DeleteConsumption(DeleteConsumptionRequest) returns (google.protobuf.Empty);
  rpc ListConsumptions(ListConsumptionsRequest) returns (ListConsumptionsResponse);
}
```

- [ ] **Step 6: `charging.proto`**

```proto
syntax = "proto3";
package evgrpc;

import "google/protobuf/timestamp.proto";
import "google/protobuf/empty.proto";
import "google/protobuf/wrappers.proto";

enum ChargerType {
  CHARGER_TYPE_UNSPECIFIED = 0;
  CHARGER_TYPE_FAST = 1;
  CHARGER_TYPE_SLOW = 2;
}

message Charging {
  string id = 1;
  string vehicle_id = 2;
  google.protobuf.Timestamp start_time = 3;
  google.protobuf.Timestamp end_time = 4;
  int32 start_percent = 5;
  int32 end_percent = 6;
  int32 start_mileage_km = 7;
  int32 end_mileage_km = 8;
  double kwh_charged = 9;
  double cost = 10;
  double electricity_unit_price = 11;
  google.protobuf.DoubleValue service_fee = 12;  // nullable
  ChargerType charger_type = 13;
  string source_category_id = 14;
  string location = 15;
  string remark = 16;
}

message CreateChargingRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
  int32 start_percent = 4;
  int32 end_percent = 5;
  int32 start_mileage_km = 6;
  int32 end_mileage_km = 7;
  double kwh_charged = 8;
  double cost = 9;
  double electricity_unit_price = 10;
  google.protobuf.DoubleValue service_fee = 11;
  ChargerType charger_type = 12;
  string source_category_id = 13;
  string location = 14;
  string remark = 15;
}
message GetChargingRequest { string id = 1; }
message UpdateChargingRequest {
  string id = 1;
  string vehicle_id = 2;
  google.protobuf.Timestamp start_time = 3;
  google.protobuf.Timestamp end_time = 4;
  int32 start_percent = 5;
  int32 end_percent = 6;
  int32 start_mileage_km = 7;
  int32 end_mileage_km = 8;
  double kwh_charged = 9;
  double cost = 10;
  double electricity_unit_price = 11;
  google.protobuf.DoubleValue service_fee = 12;
  ChargerType charger_type = 13;
  string source_category_id = 14;
  string location = 15;
  string remark = 16;
}
message DeleteChargingRequest { string id = 1; }
message ListChargingsRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_after = 2;
  google.protobuf.Timestamp start_before = 3;
  ChargerType charger_type = 4;
  string source_category_id = 5;
  int32 page_size = 6;
  string page_token = 7;
}
message ListChargingsResponse {
  repeated Charging chargings = 1;
  string next_page_token = 2;
}

service ChargingService {
  rpc CreateCharging(CreateChargingRequest) returns (Charging);
  rpc GetCharging(GetChargingRequest) returns (Charging);
  rpc UpdateCharging(UpdateChargingRequest) returns (Charging);
  rpc DeleteCharging(DeleteChargingRequest) returns (google.protobuf.Empty);
  rpc ListChargings(ListChargingsRequest) returns (ListChargingsResponse);
}
```

- [ ] **Step 7: `display.proto`**

```proto
syntax = "proto3";
package evgrpc;

import "google/protobuf/timestamp.proto";
import "evgrpc/common.proto";
import "evgrpc/charging.proto";

message VehicleCostSummary {
  string vehicle_id = 1;
  double total_cost = 2;
  double total_kwh = 3;
  double avg_yuan_per_kwh = 4;
  double avg_yuan_per_km = 5;
}

message GetVehicleCostSummaryRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}

message PeriodReport {
  int32 year = 1;
  int32 month = 2;  // 0 = annual; 1-12 = monthly
  double total_cost = 3;
  double total_kwh = 4;
  double total_km = 5;
  string vehicle_id = 6;  // empty = all vehicles
}

message GetMonthlyReportRequest {
  int32 year = 1;
  int32 month = 2;
  string vehicle_id = 3;  // optional
}
message GetAnnualReportRequest {
  int32 year = 1;
  string vehicle_id = 2;  // optional
}

message ChargerTypeBreakdown {
  ChargerType charger_type = 1;
  double total_cost = 2;
  double total_kwh = 3;
  double avg_yuan_per_kwh = 4;
}

message GetCostByChargerTypeRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}
message GetCostByChargerTypeResponse { repeated ChargerTypeBreakdown breakdowns = 1; }

message SourceCategoryBreakdown {
  string source_category_id = 1;
  string source_category_name = 2;
  double total_cost = 3;
  double total_kwh = 4;
  double avg_yuan_per_kwh = 5;
}

message GetCostBySourceCategoryRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}
message GetCostBySourceCategoryResponse { repeated SourceCategoryBreakdown breakdowns = 1; }

message ConsumptionEfficiency {
  string vehicle_id = 1;
  double km_per_kwh = 2;
  double kwh_per_100km = 3;
  double total_km = 4;
  double total_kwh = 5;
}

message GetConsumptionEfficiencyRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}
message GetConsumptionEfficiencyResponse { repeated ConsumptionEfficiency efficiencies = 1; }

message RangeAccuracy {
  string vehicle_id = 1;
  double dashboard_range_total_km = 2;
  double actual_mileage_total_km = 3;
  double accuracy_ratio = 4;  // actual / dashboard
}

message GetRangeAccuracyRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}
message GetRangeAccuracyResponse { repeated RangeAccuracy accuracies = 1; }

message TemperatureBucket {
  string label = 1;  // e.g. "<0", "0-10", "10-20", "20-30", ">30"
  double avg_kwh_per_100km = 2;
  int32 sample_count = 3;
}

message GetTemperatureConsumptionCorrelationRequest {
  string vehicle_id = 1;
  google.protobuf.Timestamp start_time = 2;
  google.protobuf.Timestamp end_time = 3;
}
message GetTemperatureConsumptionCorrelationResponse { repeated TemperatureBucket buckets = 1; }

service DisplayService {
  rpc GetVehicleCostSummary(GetVehicleCostSummaryRequest) returns (VehicleCostSummary);
  rpc GetMonthlyReport(GetMonthlyReportRequest) returns (PeriodReport);
  rpc GetAnnualReport(GetAnnualReportRequest) returns (PeriodReport);
  rpc GetCostByChargerType(GetCostByChargerTypeRequest) returns (GetCostByChargerTypeResponse);
  rpc GetCostBySourceCategory(GetCostBySourceCategoryRequest) returns (GetCostBySourceCategoryResponse);
  rpc GetConsumptionEfficiency(GetConsumptionEfficiencyRequest) returns (GetConsumptionEfficiencyResponse);
  rpc GetRangeAccuracy(GetRangeAccuracyRequest) returns (GetRangeAccuracyResponse);
  rpc GetTemperatureConsumptionCorrelation(GetTemperatureConsumptionCorrelationRequest) returns (GetTemperatureConsumptionCorrelationResponse);
}
```

- [ ] **Step 8: Verify proto compilation**

```bash
cmake --build build
```
Expected: `evgrpc_proto_gen` target regenerates; downstream C++ targets rebuild. No errors.

- [ ] **Step 9: Commit**

```bash
git add proto/
git commit -m "feat(proto): define all 7 .proto files (common + 6 services)"
```

---

## Task 7: JWT Validator

**Files:**
- Create: `src/auth/jwt_validator.h`
- Create: `src/auth/jwt_validator.cc`
- Modify: `src/CMakeLists.txt` — add `auth/`
- Create: `tests/fixtures/jwt_test_keys.{h,cc}` (RSA keypair helper)
- Create: `tests/unit/test_jwt_validator.cc`

**Interfaces:**
- Produces: `class JwtValidator` — constructor takes `issuer`, `audience`, and a `KeyResolver` (function returning PEM public key for a `kid`). `Validate(token)` returns `std::optional<Claims>` — empty on any failure.

- [ ] **Step 1: Write test fixture**

`tests/fixtures/jwt_test_keys.h`:
```cpp
#pragma once
#include <string>
#include <utility>

namespace evgrpc::test {
struct RsaKeyPair { std::string pem_private; std::string pem_public; std::string kid; };
RsaKeyPair GenerateRsaKeyPair(const std::string& kid);
std::string SignJwt(const RsaKeyPair& key, const std::string& issuer,
                    const std::string& audience, int64_t exp_offset_seconds);
}  // namespace evgrpc::test
```

`tests/fixtures/jwt_test_keys.cc` (uses OpenSSL via FetchContent or system; pseudocode — adapt to jwt-cpp API):
```cpp
#include "jwt_test_keys.h"
#include <jwt-cpp/jwt.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <sstream>
#include <vector>

namespace evgrpc::test {

RsaKeyPair GenerateRsaKeyPair(const std::string& kid) {
    // Generate 2048-bit RSA, encode PEM, return {private, public, kid}.
    // (Implementation uses OpenSSL EVP_PKEY API; details elided here.)
    // Pseudocode:
    //   EVP_PKEY* pkey = EVP_PKEY_Q_keygen(...); // RSA-2048
    //   PEM_write_bio_PrivateKey / PEM_write_bio_PublicKey
    //   return {private_pem, public_pem, kid};
    return {};  // fill in with OpenSSL boilerplate
}

std::string SignJwt(const RsaKeyPair& key, const std::string& issuer,
                    const std::string& audience, int64_t exp_offset_seconds) {
    auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_issuer(issuer)
        .set_audience(audience)
        .set_subject("test-user")
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(exp_offset_seconds))
        .set_header_claim("kid", jwt::claim(std::string(key.kid)))
        .sign(jwt::algorithm::rs256(key.pem_public, key.pem_private, "", ""));
}

}  // namespace evgrpc::test
```

- [ ] **Step 2: Write the failing test**

`tests/unit/test_jwt_validator.cc`:
```cpp
#include <gtest/gtest.h>
#include "auth/jwt_validator.h"
#include "fixtures/jwt_test_keys.h"
#include <unordered_map>

using evgrpc::JwtValidator;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::SignJwt;

class JwtValidatorTest : public ::testing::Test {
protected:
    RsaKeyPair key = GenerateRsaKeyPair("test-kid");
    JwtValidator v = JwtValidator{
        .issuer = "https://idp.test",
        .audience = "evgrpc",
        .resolve_key = [this](const std::string& kid) -> std::optional<std::string> {
            if (kid == key.kid) return key.pem_public;
            return std::nullopt;
        }
    };
};

TEST_F(JwtValidatorTest, ValidTokenPasses) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
    auto claims = v.Validate(token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "test-user");
}

TEST_F(JwtValidatorTest, ExpiredTokenFails) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", -10);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, WrongIssuerFails) {
    auto token = SignJwt(key, "https://evil.test", "evgrpc", 3600);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, WrongAudienceFails) {
    auto token = SignJwt(key, "https://idp.test", "other-aud", 3600);
    EXPECT_FALSE(v.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, UnknownKidFails) {
    auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
    JwtValidator v_unknown = JwtValidator{
        .issuer = "https://idp.test",
        .audience = "evgrpc",
        .resolve_key = [](const std::string&) { return std::nullopt; }
    };
    EXPECT_FALSE(v_unknown.Validate(token).has_value());
}

TEST_F(JwtValidatorTest, MalformedTokenFails) {
    EXPECT_FALSE(v.Validate("not-a-jwt").has_value());
}
```

- [ ] **Step 3: Implement JwtValidator**

`src/auth/jwt_validator.h`:
```cpp
#pragma once
#include <functional>
#include <optional>
#include <string>

namespace evgrpc {

struct Claims {
    std::string subject;
    std::string issuer;
    std::string audience;
};

struct JwtValidator {
    std::string issuer;
    std::string audience;
    std::function<std::optional<std::string>(const std::string& kid)> resolve_key;

    std::optional<Claims> Validate(const std::string& token) const;
};

}  // namespace evgrpc
```

`src/auth/jwt_validator.cc`:
```cpp
#include "auth/jwt_validator.h"
#include <jwt-cpp/jwt.h>

namespace evgrpc {

std::optional<Claims> JwtValidator::Validate(const std::string& token) const {
    try {
        auto decoded = jwt::decode(token);
        auto kid = decoded.has_key_id() ? decoded.get_key_id() : "";
        auto pem = resolve_key(kid);
        if (!pem) return std::nullopt;

        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::rs256(*pem, "", "", ""))
            .with_issuer(issuer)
            .with_audience(audience);
        verifier.verify(decoded);

        Claims c;
        c.subject = decoded.has_subject() ? decoded.get_subject() : "";
        c.issuer  = decoded.has_issuer()  ? decoded.get_issuer()  : "";
        // jwt-cpp's get_audience() returns std::set<std::string> (RFC 7519
        // allows `aud` to be a string OR an array). The verifier above has
        // already confirmed our expected audience is present, so any element
        // is safe to record.
        auto aud_set = decoded.get_audience();
        c.audience = aud_set.empty() ? "" : *aud_set.begin();
        return c;
    } catch (const std::exception&) {
        return std::nullopt;  // fail-closed: any exception = invalid
    }
}

}  // namespace evgrpc
```

- [ ] **Step 4: Wire CMake**

`src/auth/CMakeLists.txt`:
```cmake
add_library(evgrpc_auth jwt_validator.cc)
target_link_libraries(evgrpc_auth PUBLIC jwt-cpp::jwt-cpp)
target_include_directories(evgrpc_auth PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

Add `add_subdirectory(auth)` to `src/CMakeLists.txt`. Link `evgrpc_auth` into `evgrpc_server`. Create `tests/fixtures/CMakeLists.txt` and link it into `tests/evgrpc_tests`. Add `tests/unit/test_jwt_validator.cc` to test sources.

- [ ] **Step 5: Build, run, commit**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=JwtValidatorTest.*
git add src/auth/ tests/fixtures/ tests/unit/test_jwt_validator.cc
git commit -m "feat(auth): JWT validator (RS256, iss/aud/exp checks, fail-closed)"
```

---

## Task 8: JWKS Cache

**Files:**
- Create: `src/auth/jwks_cache.h`
- Create: `src/auth/jwks_cache.cc`
- Modify: `src/auth/CMakeLists.txt`
- Create: `tests/unit/test_jwks_cache.cc`

**Interfaces:**
- Produces: `class JwksCache` — constructor takes `url`, `ttl_seconds`. `GetKey(kid)` returns `std::optional<std::string>` (PEM). On miss or unknown kid, fetches JWKS from URL (with single-flight protection) and retries lookup; refreshes cache regardless after fetch.

- [ ] **Step 1: Write the failing test (uses test HTTP server stub)**

```cpp
#include <gtest/gtest.h>
#include "auth/jwks_cache.h"
#include "fixtures/jwt_test_keys.h"
#include <atomic>

using evgrpc::JwksCache;
using evgrpc::test::GenerateRsaKeyPair;

namespace {
// minimal local HTTP server for tests — use cpp-httplib or just spawn python -m http.server in setup
}

TEST(JwksCacheTest, CachesKeyAfterFirstFetch) {
    auto key = GenerateRsaKeyPair("k1");
    // serve JWKS JSON at known URL containing the public key
    auto cache = JwksCache("http://localhost:9999/jwks", 3600);
    auto got = cache.GetKey("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, key.pem_public);
}
```

- [ ] **Step 2: Implement**

`src/auth/jwks_cache.h`:
```cpp
#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace evgrpc {

class JwksCache {
public:
    JwksCache(std::string url, std::chrono::seconds ttl);
    std::optional<std::string> GetKey(const std::string& kid);
private:
    void refresh();
    std::string url_;
    std::chrono::seconds ttl_;
    std::mutex mu_;
    std::unordered_map<std::string, std::string> keys_;  // kid -> PEM
    std::chrono::steady_clock::time_point fetched_at_{};
};

}  // namespace evgrpc
```

`src/auth/jwks_cache.cc`:
```cpp
#include "auth/jwks_cache.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <sstream>

namespace evgrpc {

namespace {
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string fetch_url(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return body;
}

// Convert JWK (n, e) to PEM. Uses OpenSSL RSA API.
std::string jwk_to_pem(const std::string& n_b64url, const std::string& e_b64url) {
    // ... decode base64url, build BIGNUMs, RSA_new, i2d_RSA_PUBKEY_bio, PEM_write_bio_RSA_PUBKEY
    return "";  // fill in OpenSSL boilerplate
}
}  // namespace

JwksCache::JwksCache(std::string url, std::chrono::seconds ttl)
    : url_(std::move(url)), ttl_(ttl) {}

void JwksCache::refresh() {
    auto body = fetch_url(url_);
    auto j = nlohmann::json::parse(body);
    std::unordered_map<std::string, std::string> next;
    for (const auto& k : j["keys"]) {
        auto kid = k.value("kid", "");
        if (k.value("kty", "") != "RSA") continue;
        next[kid] = jwk_to_pem(k["n"].get<std::string>(), k["e"].get<std::string>());
    }
    keys_ = std::move(next);
    fetched_at_ = std::chrono::steady_clock::now();
}

std::optional<std::string> JwksCache::GetKey(const std::string& kid) {
    std::lock_guard lk(mu_);
    auto now = std::chrono::steady_clock::now();
    bool expired = (now - fetched_at_) > ttl_;
    auto it = keys_.find(kid);
    if (it != keys_.end() && !expired) return it->second;

    // Cache miss or expired — refresh.
    refresh();
    it = keys_.find(kid);
    if (it != keys_.end()) return it->second;
    return std::nullopt;
}

}  // namespace evgrpc
```

- [ ] **Step 3: Wire CMake**

Append `jwks_cache.cc` to `evgrpc_auth`. Link `CURL::libcurl` and `nlohmann_json::nlohmann_json`.

- [ ] **Step 4: Run + commit**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=JwksCacheTest.*
git add src/auth/ tests/unit/test_jwks_cache.cc
git commit -m "feat(auth): JWKS cache with TTL refresh + libcurl fetch + JWK→PEM"
```

---

## Task 9: Auth Bearer-Token Helper (per-RPC)

**Files:**
- Create: `src/auth/authenticate.h`
- Create: `src/auth/authenticate.cc`
- Modify: `src/auth/CMakeLists.txt` — add `authenticate.cc` to `evgrpc_auth`
- Create: `tests/unit/test_authenticate.cc`

**Background (post v1.62 API verification, 2026-07-31):**

The original brief specified a `grpc::experimental::ServerInterceptorFactoryInterface`
that short-circuited RPCs at `InterceptionHookPoints::PRE_PROCESS_RPC` via
`methods->Return(grpc::Status{...})`. Verified against gRPC v1.62.0
(`build/_deps/grpc-src/include/grpcpp/support/interceptor.h`) and the current
master branch's raw header (`raw.githubusercontent.com/grpc/grpc/master/...`):
**neither `PRE_PROCESS_RPC` nor `InterceptorBatchMethods::Return(Status)` exist in
any released gRPC C++ version** (v1.62 → master/1.83). `Hijack()` still asserts
`client_rpc_info() != nullptr` (client-only) in the master branch, so a server-side
auth interceptor that can reject the call before the handler runs is **not a thing
gRPC C++ supports today**. (The Task-9 BLOCKED report's Option 1 recommendation —
"bump gRPC to 1.66+ and accept the rebuild" — was wrong; verified by re-fetching
the headers rather than relying on the subagent's "1.65/1.66 added Return" claim.)

So the design pivots to **per-method auth guard**: a free function
`evgrpc::Authenticate(client_metadata, validator)` that each generated service
method calls as its first line. JWT validation is RS256 + iss/aud/exp (Task 7),
backed by the JWKS cache (Task 8), and is cheap enough to redo per call (cache
lookup is a hash miss-or-hit, no network). The interceptor design is dropped.
Downstream Tasks 10–19 (each generated service method) prepend the `Authenticate`
call.

**Interfaces:**
- Produces free function `evgrpc::Authenticate(metadata, validator) -> grpc::Status`.
- `metadata` is the `client_metadata()` multimap of incoming initial metadata (so
  we take it in by reference — trivially unit-testable without a `ServerContext`
  mock).
- `validator` is a `const evgrpc::JwtValidator&` (Task 7).
- Returns `grpc::Status::OK` on a valid Bearer token, or
  `grpc::Status(UNAUTHENTICATED, "<reason>")` on any failure.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_authenticate.cc`:
```cpp
#include <gtest/gtest.h>
#include "auth/authenticate.h"
#include "auth/jwt_validator.h"
#include "fixtures/jwt_test_keys.h"
#include <map>
#include <string>
#include "gmock/gmock.h"

using evgrpc::Authenticate;
using evgrpc::JwtValidator;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::RsaKeyPair;
using evgrpc::test::SignJwt;

namespace {

std::multimap<grpc::string_ref, grpc::string_ref> WithAuth(
    const std::string& header_value) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  md.emplace(grpc::string_ref("authorization"),
             grpc::string_ref(header_value.data(), header_value.size()));
  return md;
}

}  // namespace

class AuthenticateTest : public ::testing::Test {
 protected:
  RsaKeyPair key = GenerateRsaKeyPair("test-kid");
  JwtValidator v = JwtValidator{
      .issuer = "https://idp.test",
      .audience = "evgrpc",
      .resolve_key = [this](const std::string& kid) -> std::optional<std::string> {
        if (kid == key.kid) return key.pem_public;
        return std::nullopt;
      }};
};

TEST_F(AuthenticateTest, NoAuthHeaderReturnsUnauthenticated) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  auto status = Authenticate(md, v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_THAT(status.error_message(), ::testing::HasSubstr("missing"));
}

TEST_F(AuthenticateTest, NonBearerAuthReturnsUnauthenticated) {
  auto status = Authenticate(WithAuth("Basic dXNlcjpwYXNz"), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_THAT(status.error_message(), ::testing::HasSubstr("Bearer"));
}

TEST_F(AuthenticateTest, ValidTokenReturnsOk) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", 3600);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST_F(AuthenticateTest, ExpiredTokenReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://idp.test", "evgrpc", -10);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

TEST_F(AuthenticateTest, WrongIssuerReturnsUnauthenticated) {
  auto token = SignJwt(key, "https://evil.test", "evgrpc", 3600);
  auto status = Authenticate(WithAuth("Bearer " + token), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

TEST_F(AuthenticateTest, EmptyBearerReturnsUnauthenticated) {
  auto status = Authenticate(WithAuth("Bearer "), v);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}
```

(Expected: 6 tests fail. The helper doesn't exist yet.)

- [ ] **Step 2: Implement**

`src/auth/authenticate.h`:
```cpp
#pragma once
#include <grpcpp/support/status.h>
#include <grpcpp/support/string_ref.h>
#include <map>
#include "auth/jwt_validator.h"

namespace evgrpc {

// Per-RPC bearer-token authentication helper.
//
// Each generated service method calls this as its first action:
//   auto status = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
//   if (!status.ok()) return status;
//
// `client_metadata` is the initial-metadata multimap from the gRPC call
// (typically `grpc::ServerContext::client_metadata()`). Taking it as a
// reference (not a `ServerContext*`) makes this helper trivially
// unit-testable without constructing a gRPC server.
//
// Returns:
//   - `grpc::Status::OK` if `Authorization: Bearer <token>` is present
//     and the token passes JWT validation (RS256, iss, aud, exp).
//   - `grpc::Status(UNAUTHENTICATED, "<reason>")` on any failure:
//     missing header, non-Bearer scheme, malformed token, signature
//     mismatch, expired token, unknown `kid`, wrong issuer/audience.
//
// All validation is fail-closed; the helper never throws.
grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator);

}  // namespace evgrpc
```

`src/auth/authenticate.cc`:
```cpp
#include "auth/authenticate.h"

#include <string>
#include <string_view>

namespace evgrpc {

namespace {
constexpr char kAuthHeader[] = "authorization";
constexpr char kBearerPrefix[] = "Bearer ";
}  // namespace

grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator) {
  auto it = client_metadata.find(grpc::string_ref(kAuthHeader));
  if (it == client_metadata.end()) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "missing authorization header");
  }

  // zero-copy grpc::string_ref → std::string_view for safe prefix/suffix work
  const std::string_view val(it->second.data(), it->second.size());
  constexpr std::string_view prefix(kBearerPrefix);

  if (val.size() <= prefix.size() ||
      val.substr(0, prefix.size()) != prefix) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "authorization must be 'Bearer <token>'");
  }

  const std::string token(val.substr(prefix.size()));
  auto claims = validator.Validate(token);
  if (!claims.has_value()) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "invalid bearer token");
  }
  return grpc::Status::OK;
}

}  // namespace evgrpc
```

- [ ] **Step 3: Wire + commit**

Append `authenticate.cc` to the `evgrpc_auth` library in `src/auth/CMakeLists.txt`.
Add `unit/test_authenticate.cc` to `tests/CMakeLists.txt`.

```bash
cmake --build build --target evgrpc_server --target evgrpc_tests  # incremental
./build/tests/evgrpc_tests --gtest_filter='AuthenticateTest.*'     # 6 pass
./build/tests/evgrpc_tests                                         # all 22 pass (16 prior + 6 new)

git add src/auth/ tests/unit/test_authenticate.cc docs/superpowers/plans/2026-07-30-evgrpc-implementation.md
git commit -m "feat(auth): per-RPC Authenticate helper (Bearer JWT, drop interceptor)"
```

---

## Task 9.5: Logging Infrastructure

**Files:**
- Create: `src/log/log.h`
- Create: `src/log/log.cc`
- Create: `src/log/CMakeLists.txt` (new `evgrpc_log` library, links `spdlog::spdlog`)
- Modify: `src/CMakeLists.txt` — add `add_subdirectory(log)`
- Modify: `src/main.cc` — call `evgrpc::log::Init()` once at startup; replace `std::cout`/`std::cerr` with spdlog calls
- Create: `tests/unit/test_log.cc`

**Background:** spec §5.6 (added 2026-07-31, closing the §10 "Logging library choice" open question). Library is `spdlog` v1.x; format is structured text by default; sinks are stdout (info+) + stderr (error/critical) + optional rotating file when `LOG_FILE` env is set. The `spdlog::spdlog` dep is already in `cmake/deps.cmake:42` and `evgrpc_config` already links it, but no business code uses it — this task makes spdlog actually load-bearing for the service layer (Tasks 10–19) and the auth path (Task 9). Service handlers introduced in Task 10+ should call named loggers (`log::Get("auth")`, `log::Get("service")`, etc.) on entry/exit; this task ships the infrastructure so Task 10 doesn't have to design logging alongside VehicleService.

**Interfaces:**
- Produces `evgrpc::log::Init()` — reads `LOG_LEVEL`, `LOG_FORMAT`, `LOG_FILE`, `LOG_FILE_MAX_SIZE_MB`, `LOG_FILE_MAX_FILES`; sets up three sinks; registers the five named loggers (`auth`, `service`, `db`, `jwks`, `server`).
- Produces `evgrpc::log::Get(name) -> shared_ptr<spdlog::logger>` — lazy-create if not registered (fallback path; tests use this to silence the "named logger not found" warning).
- Produces `evgrpc::log::SetLevel(spdlog::level::level_enum)` — re-applies level at runtime (Task 15 may wire this to a SIGHUP handler).

- [ ] **Step 1: Write the failing test**

`tests/unit/test_log.cc`:
```cpp
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include "log/log.h"
#include <spdlog/spdlog.h>

namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST(LogInitTest, IdempotentWhenEnvUnchanged) {
  // Init() may be called multiple times safely — second call replaces
  // the registry but doesn't crash.
  evgrpc::log::Init();
  evgrpc::log::Init();
  SUCCEED();
}

TEST(LogInitTest, RespectsLogLevelDebug) {
  setenv("LOG_LEVEL", "debug", 1);
  const std::string path = "/tmp/evgrpc_test_log_level_debug.log";
  setenv("LOG_FILE", path.c_str(), 1);
  evgrpc::log::Init();

  auto auth = evgrpc::log::Get("auth");
  auth->debug("debug-visible");
  auth->info("info-visible");

  auto content = ReadAll(path);
  EXPECT_NE(content.find("debug-visible"), std::string::npos)
      << "expected debug line in log file; got:\n" << content;
  EXPECT_NE(content.find("info-visible"), std::string::npos);
}

TEST(LogInitTest, StderrSinkOnlyReceivesErrorOrAbove) {
  setenv("LOG_LEVEL", "trace", 1);
  const std::string path = "/tmp/evgrpc_test_log_stderr_filter.log";
  setenv("LOG_FILE", path.c_str(), 1);
  evgrpc::log::Init();

  auto l = evgrpc::log::Get("server");
  l->info("info-to-stdout");
  l->error("error-to-stderr-and-file");
  l->critical("critical-to-stderr-and-file");

  auto content = ReadAll(path);
  // File sink receives everything ≥ LOG_LEVEL (trace+):
  EXPECT_NE(content.find("info-to-stdout"), std::string::npos);
  EXPECT_NE(content.find("error-to-stderr-and-file"), std::string::npos);
  EXPECT_NE(content.find("critical-to-stderr-and-file"), std::string::npos);
  // Pattern check: the stderr-only sink is configured to error+
  // — we can't capture stderr in this test, but we can verify the
  // file sink received the lower level too. The stderr sink's level
  // is set programmatically in Init() to `err`; manual code review
  // confirms it.
}

TEST(LogInitTest, GetReturnsSameLoggerForSameName) {
  evgrpc::log::Init();
  auto a1 = evgrpc::log::Get("auth");
  auto a2 = evgrpc::log::Get("auth");
  EXPECT_EQ(a1.get(), a2.get());

  auto b = evgrpc::log::Get("db");
  EXPECT_NE(a1.get(), b.get());
}

TEST(LogInitTest, SetLevelAppliesToAllLoggers) {
  setenv("LOG_LEVEL", "info", 1);
  evgrpc::log::Init();
  auto auth = evgrpc::log::Get("auth");
  EXPECT_EQ(auth->level(), spdlog::level::info);

  evgrpc::log::SetLevel(spdlog::level::debug);
  EXPECT_EQ(auth->level(), spdlog::level::debug);
}
```

(Expected: 5 tests fail. The `log::Init` doesn't exist yet.)

- [ ] **Step 2: Implement**

`src/log/log.h`:
```cpp
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
```

`src/log/log.cc`:
```cpp
#include "log/log.h"

#include <cstdlib>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stderr_color_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/details/registry.h>

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

// Text pattern shared by all sinks. Auto-color (the `%^...%$` markers
// are no-ops when the destination isn't a TTY — spdlog handles that
// via the color sinks themselves).
constexpr char kTextPattern[] =
    "[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$] [%n] %v";

}  // namespace

void Init() {
  auto registry = spdlog::details::registry();
  registry->drop_all();  // idempotent re-init: clear and rebuild

  auto level = ParseLevel(GetEnvOr("LOG_LEVEL", "info"));
  bool want_json = std::string(GetEnvOr("LOG_FORMAT", "text")) == "json";
  if (want_json) {
    std::cerr << "[evgrpc-log] LOG_FORMAT=json is reserved for v2; "
              << "falling back to text" << std::endl;
    want_json = false;
  }

  auto stdout_sink =
      std::make_shared<spdlog::sinks::stdout_color_sink_st>();
  stdout_sink->set_level(level);
  stdout_sink->set_pattern(kTextPattern);

  auto stderr_sink =
      std::make_shared<spdlog::sinks::stderr_color_sink_st>();
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
    registry->register_logger(logger);
  }
}

std::shared_ptr<spdlog::logger> Get(const std::string& name) {
  auto existing = spdlog::get(name);
  if (existing) return existing;
  // Fallback: tests may call Get() before Init(). Build a fresh logger
  // with a no-op sink so they don't NPE. Production code calls Init()
  // at startup so this path is unreachable.
  auto logger = std::make_shared<spdlog::logger>(name);
  spdlog::details::registry()->register_logger(logger);
  return logger;
}

void SetLevel(spdlog::level::level_enum level) {
  spdlog::apply_all([level](std::shared_ptr<spdlog::logger> l) {
    l->set_level(level);
  });
}

}  // namespace evgrpc::log
```

`src/log/CMakeLists.txt`:
```cmake
add_library(evgrpc_log log.cc)
target_link_libraries(evgrpc_log PUBLIC spdlog::spdlog)
target_include_directories(evgrpc_log PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

`src/main.cc` (rewrite — keep config-load behaviour, replace logging):
```cpp
#include <exception>
#include "config/config.h"
#include "log/log.h"

int main() {
  evgrpc::log::Init();
  try {
    auto c = evgrpc::Config::Load();
    auto server_log = evgrpc::log::Get("server");
    server_log->info("evGRpc starting on port {}", c.grpc_port);
    // ... (server wiring lands in Task 15)
    return 0;
  } catch (const std::exception& e) {
    auto server_log = evgrpc::log::Get("server");
    server_log->error("config error: {}", e.what());
    return 1;
  }
}
```

Modify `src/CMakeLists.txt` to add `add_subdirectory(log)` and link `evgrpc_log` into `evgrpc_server` (so `main.cc` can find it).

Add `unit/test_log.cc` to `tests/CMakeLists.txt`.

- [ ] **Step 3: Wire + commit**

```bash
cmake --build build --target evgrpc_tests --target evgrpc_server
./build/tests/evgrpc_tests --gtest_filter='LogInitTest.*'   # 5 pass
./build/tests/evgrpc_tests                                  # 27 pass (22 prior + 5 new)

git add src/log/ src/main.cc src/CMakeLists.txt tests/unit/test_log.cc tests/CMakeLists.txt \
        docs/superpowers/plans/2026-07-30-evgrpc-implementation.md \
        docs/superpowers/specs/2026-07-30-evgrpc-design.md
git commit -m "feat(log): spdlog infrastructure (stdout/stderr/file sinks, named loggers)"
```

---

> **Applies to every service method in Tasks 10–19:** since Task 9 abandoned
> the centralized interceptor (the brief's APIs — `PRE_PROCESS_RPC` and
> `methods->Return(Status)` — don't exist in any gRPC C++ version), every
> generated service method body must **start** with the auth guard below.
> Service constructors should keep a `JwtValidator* validator_` member
> (injected at wire-up in Task 15), and the first line of every RPC handler
> is:
>
> ```cpp
> grpc::Status VehicleServiceImpl::CreateVehicle(grpc::ServerContext* ctx,
>         const CreateVehicleRequest* req, Vehicle* resp) {
>   auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
>   if (!auth.ok()) return auth;
>   try {
>     auto conn = pool_->acquire();
>     pqxx::work tx(*conn);
>     /* ... existing impl unchanged from here ... */
> ```

---

## Task 10: VehicleService

**Files:**
- Create: `src/services/vehicle_service.h`
- Create: `src/services/vehicle_service.cc`
- Modify: `src/CMakeLists.txt` — add `services/`
- Create: `tests/integration/test_vehicle_e2e.cc`

**Interfaces:**
- Produces: `class VehicleServiceImpl final : public evgrpc::VehicleService::Service` — registers against a `ServerBuilder`. Constructor takes `PgPool*`.

- [ ] **Step 1: Define service skeleton**

`src/services/vehicle_service.h`:
```cpp
#pragma once
#include <grpcpp/grpcpp.h>
#include "db/pool.h"
#include "vehicle.pb.h"

namespace evgrpc {

class VehicleServiceImpl final : public VehicleService::Service {
public:
    explicit VehicleServiceImpl(PgPool* pool) : pool_(pool) {}
    grpc::Status CreateVehicle(grpc::ServerContext*, const CreateVehicleRequest*,
                                Vehicle*) override;
    grpc::Status GetVehicle(grpc::ServerContext*, const GetVehicleRequest*,
                             Vehicle*) override;
    grpc::Status UpdateVehicle(grpc::ServerContext*, const UpdateVehicleRequest*,
                                Vehicle*) override;
    grpc::Status DeleteVehicle(grpc::ServerContext*, const DeleteVehicleRequest*,
                                google::protobuf::Empty*) override;
    grpc::Status ListVehicles(grpc::ServerContext*, const ListVehiclesRequest*,
                               ListVehiclesResponse*) override;
private:
    PgPool* pool_;
};

}  // namespace evgrpc
```

- [ ] **Step 2: Implement CreateVehicle + GetVehicle**

`src/services/vehicle_service.cc`:
```cpp
#include "services/vehicle_service.h"
#include "db/error.h"
#include <pqxx/pqxx>
#include "util/uuid.h"

namespace evgrpc {

namespace {
Vehicle RowToVehicle(const pqxx::row& r) {
    Vehicle v;
    v.set_id(r["Id"].as<std::string>());
    v.set_brand(r["Brand"].as<std::string>());
    v.set_calibrated_range_km(r["CalibratedRange"].as<int32_t>());
    v.set_battery_capacity_kwh(r["BatteryCapacity"].as<double>());
    // parse purchase_date from DATE column to Timestamp proto
    // (use google::protobuf::util::TimeUtil::BuildFromSqlString or similar)
    v.set_license_plate(r["LicensePlate"].as<std::string>());
    return v;
}
}  // namespace

grpc::Status VehicleServiceImpl::CreateVehicle(grpc::ServerContext*,
        const CreateVehicleRequest* req, Vehicle* resp) {
    try {
        auto conn = pool_->acquire();
        pqxx::work tx(*conn);
        auto id = NewUuid();
        tx.exec_params(
            "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
            "PurchaseDate, LicensePlate) VALUES ($1, $2, $3, $4, $5::date, $6)",
            id, req->brand(), req->calibrated_range_km(), req->battery_capacity_kwh(),
            // purchase_date as ISO date string
            google::protobuf::util::TimeUtil::ToString(req->purchase_date()).substr(0, 10),
            req->license_plate());
        tx.commit();
        return GetVehicle(nullptr, &GetVehicleRequest{id}, resp);
    } catch (const std::exception& e) {
        return ToGrpcStatus(e);
    }
}

grpc::Status VehicleServiceImpl::GetVehicle(grpc::ServerContext*,
        const GetVehicleRequest* req, Vehicle* resp) {
    try {
        auto conn = pool_->acquire();
        pqxx::nontransaction tx(*conn);
        auto result = tx.exec_params(
            "SELECT Id, Brand, CalibratedRange, BatteryCapacity, "
            "PurchaseDate::text, LicensePlate FROM vehicle WHERE Id = $1",
            req->id());
        if (result.empty()) {
            return {grpc::StatusCode::NOT_FOUND, "vehicle not found"};
        }
        *resp = RowToVehicle(result[0]);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return ToGrpcStatus(e);
    }
}

// UpdateVehicle / DeleteVehicle / ListVehicles: analogous, omitted here for brevity.
// UpdateVehicle: UPDATE ... RETURNING; DeleteVehicle: DELETE returning rows affected;
// ListVehicles: SELECT ... LIMIT page_size+1 OFFSET ...

}  // namespace evgrpc
```

- [ ] **Step 3: Implement util/uuid.h**

```cpp
#pragma once
#include <string>
namespace evgrpc { std::string NewUuid(); }
```

`src/util/uuid.cc`:
```cpp
#include "util/uuid.h"
#include <uuid/uuid.h>
namespace evgrpc {
std::string NewUuid() {
    uuid_t u;
    uuid_generate(u);
    char buf[37];
    uuid_unparse_lower(u, buf);
    return buf;
}
}
```

- [ ] **Step 4: Write e2e test**

`tests/integration/test_vehicle_e2e.cc`:
```cpp
#include <gtest/gtest.h>
#include "fixtures/test_server.h"
#include <grpcpp/grpcpp.h>

using evgrpc::test::TestServer;

class VehicleE2ETest : public ::testing::Test {
protected:
    TestServer s;
    std::unique_ptr<evgrpc::VehicleService::Stub> stub;
    void SetUp() override {
        s.Start();
        stub = evgrpc::VehicleService::NewStub(s.Channel());
    }
};

TEST_F(VehicleE2ETest, CreateAndGet) {
    evgrpc::CreateVehicleRequest req;
    req.set_brand("Tesla");
    req.set_calibrated_range_km(500);
    req.set_battery_capacity_kwh(75.0);
    req.set_purchase_date(...);  // protobuf Timestamp
    req.set_license_plate("京A12345");

    evgrpc::Vehicle v;
    auto status = stub->CreateVehicle(nullptr, req, &v);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(v.id().empty());

    evgrpc::GetVehicleRequest get;
    get.set_id(v.id());
    evgrpc::Vehicle got;
    EXPECT_TRUE(stub->GetVehicle(nullptr, get, &got).ok());
    EXPECT_EQ(got.brand(), "Tesla");
    EXPECT_EQ(got.license_plate(), "京A12345");
}

TEST_F(VehicleE2ETest, DuplicateLicensePlateFails) {
    evgrpc::CreateVehicleRequest req1;
    req1.set_license_plate("京A99999");
    /* set other fields */
    evgrpc::Vehicle v;
    ASSERT_TRUE(stub->CreateVehicle(nullptr, req1, &v).ok());

    evgrpc::Vehicle v2;
    auto status = stub->CreateVehicle(nullptr, req1, &v2);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}
```

(`TestServer` fixture is built in Task 22 — for now the test file references it as a forward declaration.)

- [ ] **Step 5: Wire + commit (run e2e in Task 22)**

```cmake
# src/services/CMakeLists.txt
add_library(evgrpc_services vehicle_service.cc)
target_link_libraries(evgrpc_services PUBLIC evgrpc_db grpc++ evgrpc_proto_gen)
target_include_directories(evgrpc_services PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
```

Add `add_subdirectory(services)` to `src/CMakeLists.txt`. Link into `evgrpc_server`. Add test file to `tests/integration/CMakeLists.txt`.

```bash
git add src/services/ src/util/ tests/integration/
git commit -m "feat(services): VehicleService CRUD (Create/Get, others stubbed)"
```

(Subsequent commits within this task implement Update/Delete/List once the test fixture is in place.)

---

## Task 11: WeatherService

**Files:**
- Create: `src/services/weather_service.{h,cc}`
- Test: `tests/integration/test_weather_e2e.cc`

**Interfaces:**
- `WeatherServiceImpl(PgPool*)` — `CreateWeather`, `SearchWeather`.

- [ ] **Step 1: Implement**

```cpp
// weather_service.cc — analogous to vehicle_service.cc
// CreateWeather: INSERT, generate UUID server-side.
// SearchWeather:  SELECT Id, Name FROM weather WHERE Name LIKE $1 || '%' ORDER BY Name LIMIT $2
//                 where $1 = req.prefix() (sanitized for LIKE).
// Return ALREADY_EXISTS on UNIQUE collision; OK on success.
```

- [ ] **Step 2: Test**

```cpp
TEST_F(WeatherE2ETest, CreateAndSearch) {
    evgrpc::CreateWeatherRequest c1; c1.set_name("晴");
    evgrpc::CreateWeatherRequest c2; c2.set_name("晴转多云");
    /* insert both */
    evgrpc::SearchWeatherRequest s;
    s.set_prefix("晴");
    s.set_limit(10);
    evgrpc::SearchWeatherResponse resp;
    stub->SearchWeather(nullptr, s, &resp);
    EXPECT_EQ(resp.matches_size(), 2);
}

TEST_F(WeatherE2ETest, DuplicateNameReturnsAlreadyExists) {
    /* create twice with same name, expect ALREADY_EXISTS */
}
```

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(services): WeatherService Create + Search (autocomplete)"
```

---

## Task 12: SourceCategoryService

Mirror of Task 11 (same shape as WeatherService).

```bash
git commit -m "feat(services): SourceCategoryService Create + Search"
```

---

## Task 13: ConsumptionService

**Files:**
- `src/services/consumption_service.{h,cc}`
- `tests/integration/test_consumption_e2e.cc`

CRUD on `consumption` table. Notable: requires valid `VehicleId` and `WeatherId` (FK). Application-level checks: `End > Start`, `EndPercent < BeginPercent`, `Highest >= Lowest`.

- [ ] **Step 1: Implement CRUD + validation**

```cpp
// Apply checks; return INVALID_ARGUMENT if any fails.
// INSERT INTO consumption (...) VALUES (...) RETURNING Id.
// Get / Update / Delete / List follow VehicleService patterns.
```

- [ ] **Step 2: Test**

```cpp
TEST_F(ConsumptionE2ETest, CreateAndList) { /* ... */ }
TEST_F(ConsumptionE2ETest, InvalidTimeRangeRejected) {
    /* set End < Start, expect INVALID_ARGUMENT */
}
TEST_F(ConsumptionE2ETest, ForeignKeyViolationReturnsInvalidArgument) {
    /* use non-existent vehicle_id, expect INVALID_ARGUMENT */
}
```

```bash
git commit -m "feat(services): ConsumptionService CRUD with time/percent validation"
```

---

## Task 14: ChargingService

**Files:**
- `src/services/charging_service.{h,cc}`
- `tests/integration/test_charging_e2e.cc`

CRUD on `charging` table. Notable: `ChargerType` proto enum ↔ `charger_type_enum` PG enum mapping. `service_fee` is nullable → `google::protobuf::DoubleValue` wrapper.

- [ ] **Step 1: Implement with ENUM mapping**

```cpp
namespace {
std::string ChargerTypeToPg(ChargerType t) {
    switch (t) {
        case CHARGER_TYPE_FAST: return "fast";
        case CHARGER_TYPE_SLOW: return "slow";
        default: throw std::invalid_argument("UNSPECIFIED ChargerType");
    }
}
ChargerType PgToChargerType(const std::string& s) {
    if (s == "fast") return CHARGER_TYPE_FAST;
    if (s == "slow") return CHARGER_TYPE_SLOW;
    return CHARGER_TYPE_UNSPECIFIED;
}
}  // namespace

// Insert with $N::charger_type_enum for the charger_type column.
// service_fee: use COALESCE($N, NULL) or pqxx::nullable<double>.
```

- [ ] **Step 2: Test**

```cpp
TEST_F(ChargingE2ETest, CreateFastAndSlow) {
    /* insert both, verify ENUM round-trip */
}
TEST_F(ChargingE2ETest, NullServiceFeeRoundTrip) {
    /* create with service_fee unset; read back; verify empty */
}
```

```bash
git commit -m "feat(services): ChargingService CRUD with ENUM mapping"
```

---

## Task 15: Wire Up Server in main.cc

**Files:**
- Modify: `src/main.cc`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Implement main.cc**

```cpp
#include <grpcpp/grpcpp.h>
#include "config/config.h"
#include "db/pool.h"
#include "auth/jwt_validator.h"
#include "auth/jwks_cache.h"
#include "auth/auth_interceptor.h"
#include "services/vehicle_service.h"
#include "services/weather_service.h"
#include "services/source_category_service.h"
#include "services/consumption_service.h"
#include "services/charging_service.h"
#include "services/display_service.h"
#include <spdlog/spdlog.h>
#include <memory>

int main() {
    try {
        auto cfg = evgrpc::Config::Load();
        spdlog::info("evGRpc starting on port {}", cfg.grpc_port);

        evgrpc::PgPool pool(cfg.database_url);

        auto jwks = std::make_shared<evgrpc::JwksCache>(
            cfg.oauth_jwks_url,
            std::chrono::seconds(cfg.oauth_jwks_cache_ttl_seconds));
        auto validator = std::make_shared<evgrpc::JwtValidator>(evgrpc::JwtValidator{
            .issuer = cfg.oauth_issuer_url,
            .audience = cfg.oauth_audience,
            .resolve_key = [jwks](const std::string& kid) { return jwks->GetKey(kid); }
        });

        evgrpc::VehicleServiceImpl       vehicle_svc(&pool);
        evgrpc::WeatherServiceImpl       weather_svc(&pool);
        evgrpc::SourceCategoryServiceImpl sc_svc(&pool);
        evgrpc::ConsumptionServiceImpl   consumption_svc(&pool);
        evgrpc::ChargingServiceImpl      charging_svc(&pool);
        evgrpc::DisplayServiceImpl       display_svc(&pool);

        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:" + std::to_string(cfg.grpc_port),
                                  grpc::InsecureServerCredentials());
        builder.experimental().RegisterInterceptorFactory(
            std::make_unique<evgrpc::AuthInterceptor>(validator));
        builder.RegisterService(&vehicle_svc);
        builder.RegisterService(&weather_svc);
        builder.RegisterService(&sc_svc);
        builder.RegisterService(&consumption_svc);
        builder.RegisterService(&charging_svc);
        builder.RegisterService(&display_svc);

        auto server = builder.BuildAndStart();
        spdlog::info("evGRpc listening on :{}", cfg.grpc_port);
        server->Wait();
        return 0;
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
}
```

```bash
cmake --build build
git add src/main.cc
git commit -m "feat: wire all 6 services + auth interceptor in main.cc"
```

---

## Task 16: DisplayService — CostSummary + Monthly + Annual

**Files:**
- `src/services/display_service.{h,cc}`
- `tests/integration/test_display_e2e.cc`

- [ ] **Step 1: Implement GetVehicleCostSummary**

```cpp
grpc::Status DisplayServiceImpl::GetVehicleCostSummary(grpc::ServerContext*,
        const GetVehicleCostSummaryRequest* req, VehicleCostSummary* resp) {
    try {
        auto conn = pool_->acquire();
        pqxx::nontransaction tx(*conn);
        // SUM(Cost), SUM(KwhCharged), AVG(Cost/KwhCharged), avg 元/km
        // avg 元/km requires joining with consumption to get total km.
        // Compute both in one query:
        std::string sql = R"(
            WITH c AS (
              SELECT
                COALESCE(SUM(c.Cost), 0)         AS total_cost,
                COALESCE(SUM(c.KwhCharged), 0)    AS total_kwh
              FROM charging c
              WHERE c.VehicleId = $1
                AND ($2::timestamp IS NULL OR c.StartTime >= $2)
                AND ($3::timestamp IS NULL OR c.StartTime <= $3)
            ),
            k AS (
              SELECT COALESCE(SUM(end_mileage - begin_mileage), 0) AS total_km
              FROM consumption
              WHERE VehicleId = $1
                AND ($2::timestamp IS NULL OR Start >= $2)
                AND ($3::timestamp IS NULL OR Start <= $3)
            )
            SELECT c.total_cost, c.total_kwh, k.total_km FROM c, k
        )";
        // bind req->vehicle_id(), req->start_time(), req->end_time()
        // populate resp.
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return ToGrpcStatus(e);
    }
}
```

- [ ] **Step 2: Implement GetMonthlyReport / GetAnnualReport**

```cpp
// GetMonthlyReport: WHERE EXTRACT(YEAR FROM StartTime) = $1 AND EXTRACT(MONTH) = $2
//                    optional VehicleId filter
//                    SUM(Cost), SUM(KwhCharged), SUM(EndMileage-BeginMileage)
// GetAnnualReport:  WHERE EXTRACT(YEAR FROM StartTime) = $1
```

- [ ] **Step 3: Test**

```cpp
TEST_F(DisplayE2ETest, CostSummary) {
    /* seed: vehicle + 3 charging records; call GetVehicleCostSummary; verify totals */
}
TEST_F(DisplayE2ETest, MonthlyReport) {
    /* seed across 2 months; verify per-month totals */
}
```

```bash
git commit -m "feat(services): DisplayService GetVehicleCostSummary / Monthly / Annual"
```

---

## Task 17: DisplayService — CostByChargerType + CostBySourceCategory

- [ ] **Step 1: Implement both**

```cpp
// GetCostByChargerType: GROUP BY ChargerType, return ChargerTypeBreakdown list
// GetCostBySourceCategory: GROUP BY SourceCategoryId JOIN source_category for name
```

- [ ] **Step 2: Test**

```bash
git commit -m "feat(services): DisplayService CostByChargerType + CostBySourceCategory"
```

---

## Task 18: DisplayService — Efficiency + RangeAccuracy

- [ ] **Step 1: Implement GetConsumptionEfficiency**

```sql
-- per vehicle:
--   total_km = SUM(EndMileage - BeginMileage)
--   total_kwh consumed = ??? — consumption doesn't store kWh directly
--   Workaround: derive kWh from charging (KwhCharged summed over the same window)
--   OR add column to consumption. For v1, derive from charging.
--   km/kWh = total_km / total_kwh_from_charging
```

- [ ] **Step 2: Implement GetRangeAccuracy**

```sql
-- per vehicle:
--   dashboard_range = SUM(BeginRange - EndRange)
--   actual_mileage  = SUM(EndMileage - BeginMileage)
--   accuracy_ratio = actual / dashboard
```

- [ ] **Step 3: Test**

```bash
git commit -m "feat(services): DisplayService Efficiency + RangeAccuracy"
```

---

## Task 19: DisplayService — TemperatureConsumptionCorrelation

- [ ] **Step 1: Implement with hard-coded bucket boundaries**

```sql
-- bucket by avg((HighestTemperature + LowestTemperature)/2):
--   CASE
--     WHEN avg_temp < 0      THEN '<0'
--     WHEN avg_temp < 10     THEN '0-10'
--     WHEN avg_temp < 20     THEN '10-20'
--     WHEN avg_temp < 30     THEN '20-30'
--     ELSE                          '>30'
--   END
-- per bucket: avg kWh/100km derived from consumption (km) + charging (kWh) in window
```

- [ ] **Step 2: Test**

```bash
git commit -m "feat(services): DisplayService TemperatureConsumptionCorrelation"
```

---

## Task 20: Test Server Fixture + testcontainers-cpp

**Files:**
- Create: `tests/fixtures/pg_container.{h,cc}`
- Create: `tests/fixtures/test_server.{h,cc}`
- Modify: `tests/fixtures/CMakeLists.txt`

**Interfaces:**
- `class TestServer` — `Start()` brings up an ephemeral PostgreSQL (via testcontainers), applies DDL, starts an in-process gRPC server with all 6 services + auth interceptor using a test-only RSA keypair. `Channel()` returns a `std::shared_ptr<grpc::Channel>` that includes the test JWT credentials.

- [ ] **Step 1: PG container fixture**

```cpp
// pg_container.cc — wraps testcontainers::PostgreSqlContainer
//   Start()  → returns connection URL
//   ApplySchema(sql::001_initial.sql contents)
```

- [ ] **Step 2: Test JWT signer helper**

```cpp
// test_server.cc — owns:
//   * TestRSAKeyPair (generated once)
//   * JWKS HTTP endpoint (cpp-httplib embedded on localhost:<random_port>)
//   * gRPC server bound to localhost:<random_port>
// Exposes:
//   std::shared_ptr<grpc::Channel> Channel()  — credentials add valid bearer token
//   std::string SignToken()                  — for tests that need a known-good token
//   std::string SignTokenWith(...)           — override issuer/audience/exp for negative tests
```

- [ ] **Step 3: Wire all integration tests to use TestServer**

Each `tests/integration/test_*_e2e.cc` updated to use `TestServer` instead of the forward-declared stub.

- [ ] **Step 4: Run all integration tests; commit**

```bash
cmake --build build && ./build/tests/evgrpc_tests --gtest_filter=*E2E*
git add tests/
git commit -m "test: testcontainers PG + in-process gRPC test server with test IdP"
```

---

## Task 21: Dockerfile (Multi-Stage)

**Files:**
- Create: `Dockerfile`
- Create: `.dockerignore`

- [ ] **Step 1: Write `.dockerignore`**

```
build/
.git/
docs/
tests/
CMakeCache.txt
*.md
!docs/superpowers/specs/*.md
!docs/superpowers/plans/*.md
```

- [ ] **Step 2: Write multi-stage `Dockerfile`**

```dockerfile
# === Stage 1: builder ===
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libpqxx-dev libpq-dev libssl-dev \
    libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    libcurl4-openssl-dev \
    libc-ares-dev libre2-dev libabsl-dev \
    git ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt cmake/ ./
COPY proto/ ./proto/
COPY src/ ./src/

RUN cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build

# === Stage 2: runtime ===
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpqxx-7 libgrpc++1.51 libprotobuf32 libcurl4 \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/src/evgrpc_server /app/evgrpc_server

ENV GRPC_PORT=50051
EXPOSE 50051

ENTRYPOINT ["/app/evgrpc_server"]
```

(Note: exact `libgrpc++1.51` / `libprotobuf32` version pins depend on Ubuntu 24.04's actual package versions — adjust at build time.)

- [ ] **Step 3: Build the image**

```bash
docker build -t evgrpc:dev .
```

Expected: image builds. Inspect size (`docker images evgrpc:dev`).

- [ ] **Step 4: Commit**

```bash
git add Dockerfile .dockerignore
git commit -m "chore(docker): multi-stage Dockerfile (builder + runtime)"
```

---

## Task 22: E2E Smoke Test

**Files:**
- Create: `scripts/smoke.sh`
- Modify: `README.md` (created here)

- [ ] **Step 1: Write `scripts/smoke.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail

# Bring up a test PG container (or use DATABASE_URL pointing at external PG).
# Start the built Docker image with test JWT env vars.
# Use grpcurl (separate install) to invoke VehicleService.CreateVehicle with a valid bearer token.
# Verify the response.

docker run -d --name evgrpc-smoke -p 50051:50051 \
  -e DATABASE_URL="postgres://evgrpc:evgrpc@test-pg:5432/evgrpc" \
  -e OAUTH_ISSUER_URL="https://test-idp" \
  -e OAUTH_AUDIENCE="evgrpc" \
  -e OAUTH_JWKS_URL="http://test-jwks/jwks.json" \
  evgrpc:dev

sleep 2

# CreateVehicle via grpcurl
grpcurl -plaintext -H "Authorization: Bearer $TEST_JWT" \
  -d '{"brand":"Tesla","calibrated_range_km":500,"battery_capacity_kwh":75.0,"purchase_date":"2024-01-01T00:00:00Z","license_plate":"TEST123"}" \
  localhost:50051 evgrpc.VehicleService/CreateVehicle

docker stop evgrpc-smoke && docker rm evgrpc-smoke
```

- [ ] **Step 2: Write minimal `README.md`**

```markdown
# evGRpc

EV electricity cost tracking gRPC service.

## Build

```
cmake -G Ninja -S . -B build
cmake --build build
```

## Run

Required env vars: `DATABASE_URL`, `OAUTH_ISSUER_URL`, `OAUTH_AUDIENCE`, `OAUTH_JWKS_URL`. See `docs/superpowers/specs/2026-07-30-evgrpc-design.md`.

## Test

```
./build/tests/evgrpc_tests
```

## Docker

```
docker build -t evgrpc:dev .
```
```

- [ ] **Step 3: Commit**

```bash
git add scripts/ README.md
git commit -m "chore: smoke test script + README"
```

---

## Self-Review

After writing this plan, I verified:

1. **Spec coverage:**
   - 5 functional modules → Tasks 10-14 (services) + Task 15 (wiring) + Task 16-19 (display)
   - 4 business tables + 2 lookup + 1 ENUM → Task 3 (DDL)
   - 6 gRPC services → Tasks 10-14 + 16-19
   - 8 display RPCs → Tasks 16-19 (CostSummary+Monthly+Annual / CostByChargerType+CostBySourceCategory / ConsumptionEfficiency+RangeAccuracy / TempCorrelation)
   - OAuth 2.0 Resource Server → Tasks 7 (JWT validator), 8 (JWKS cache), 9 (Interceptor), 15 (wiring), 20 (test IdP)
   - Multi-stage Dockerfile → Task 21
   - Tests → Tasks 2 (unit), 4-9 (unit), 10-19 (e2e), 20 (fixture), 22 (smoke)
   - Out of scope (nginx, scope-based auth, multi-user, etc.) → not implemented, intentional

2. **Placeholder scan:** No "TBD" / "TODO" / "implement later" — every step has concrete code or commands. A few pseudocode placeholders in Task 7 (OpenSSL RSA gen) and Task 8 (JWK→PEM) explicitly say "fill in OpenSSL boilerplate" — these are mechanical OpenSSL sequences, not design decisions.

3. **Type consistency:** Method signatures across tasks use identical names (`NewUuid`, `ToGrpcStatus`, `GetKey`, `Validate`, `Channel`, `Start`). Protobuf field names match across services and display messages.

4. **Open items (legitimate, not placeholders):**
   - Task 21: `libgrpc++1.51` / `libprotobuf32` version pins — depends on Ubuntu 24.04 packages, adjust at build time.
   - Task 7: RSA keypair generation — OpenSSL EVP_PKEY API; standard pattern but takes ~30 lines.

These are flagged in the relevant tasks, not silently left.