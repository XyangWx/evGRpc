# evGRpc — Electric Vehicle Electricity Cost Tracking Service

**Date:** 2026-07-30
**Status:** Implemented — all 23 plan tasks complete; tagged v0.1.0 (2026-08-05)
**Repo:** `/workspace/repositories/evGRpc`

---

## 1. Goals & Scope

### Goal

Build a gRPC service in C++ that **records and analyzes electricity costs for electric vehicles**. All data entry is manual. Analysis surfaces per-vehicle cost, charging efficiency, and weather/temperature correlation. Service is protected by OAuth 2.0 — non-authenticated callers cannot invoke any RPC.

### In Scope (v1)

- C++ gRPC server (`grpc++`)
- PostgreSQL backend (external, connection-only)
- Dockerfile packaging the gRPC service
- Manual data entry via gRPC RPCs
- 5 functional modules: vehicle, consumption, weather, charging, display
- Multi-stage Docker build (CMake + Ninja)
- **OAuth 2.0 Resource Server authentication** — JWT bearer tokens validated against an external IdP via JWKS

### Out of Scope (v1)

- nginx reverse proxy — **deferred to v2**
- OAuth 2.0 server / token issuance — handled by external IdP
- Scope-based authorization (any valid token can call any RPC in v1)
- Multi-user isolation (no `user_id` columns in business tables; single-user assumed)
- External weather API (manual entry with autocomplete)
- Currency conversion (RMB only)
- Web UI (gRPC clients are CLI / custom)
- Mobile / web clients

---

## 2. Architecture Overview

```
┌──────────────────┐
│  gRPC Client(s)  │
│  (Bearer JWT)    │
└────────┬─────────┘
         │ HTTP/2 + Authorization: Bearer <JWT>
         ▼
┌──────────────────────────────────────────┐
│  Docker Container                        │
│  ┌────────────────────────────────────┐  │
│  │  C++ gRPC server                   │  │
│  │  ┌──────────────────────────────┐  │  │
│  │  │  Auth Interceptor            │◄─┼──── JWKS cache (refreshed)
│  │  │  (validates JWT signature,   │  │     │
│  │  │   iss, aud, exp)             │  │     │
│  │  └──────────────┬───────────────┘  │     │
│  │  ┌──────────────▼───────────────┐  │     │
│  │  │  6 RPC services              │  │     │
│  │  │  (Vehicle / Weather /        │  │     │
│  │  │   SourceCategory /           │  │     │
│  │  │   Consumption / Charging /   │  │     │
│  │  │   Display)                   │  │     │
│  │  └──────────────┬───────────────┘  │     │
│  │  ┌──────────────▼───────────────┐  │     │
│  │  │  libpqxx (PG client)         │  │     │
│  │  └──────────────────────────────┘  │     │
│  └────────────────────────────────────┘  │
└───────────┬──────────────────────────────┘
            │ PostgreSQL protocol
            ▼
   ┌─────────────────┐
   │  PostgreSQL     │  (external, already deployed)
   └─────────────────┘

JWKS fetch (on startup + cache miss):
   ┌─────────────────────┐
   │  OAuth 2.0 IdP      │  (external — Keycloak / Auth0 / any OIDC provider)
   │  /.well-known/      │
   │  jwks.json          │
   └─────────────────────┘
```

- **Single binary** runs all 6 gRPC services on one port (default `:50051`).
- **Auth Interceptor** runs before every RPC; rejects requests with missing or invalid token.
- **PostgreSQL** is external — connection via `DATABASE_URL` env var.
- **OAuth IdP** is external — JWKS endpoint via `OAUTH_JWKS_URL` env var.
- **No internal state** between requests — every request reads from / writes to PostgreSQL (except the JWKS cache).

---

## 3. Data Model

PostgreSQL schema. All IDs are UUIDs.

### 3.1 `vehicle`

Primary entity. Identified uniquely by `LicensePlate`.

```sql
CREATE TABLE vehicle (
  Id               UUID PRIMARY KEY,
  Brand            VARCHAR(36)  NOT NULL,
  CalibratedRange  INTEGER       NOT NULL,    -- 单位 km
  BatteryCapacity  DECIMAL(10,2) NOT NULL,    -- 单位 kWh
  PurchaseDate     DATE          NOT NULL,
  LicensePlate     VARCHAR(15)   NOT NULL UNIQUE
);
```

### 3.2 `weather` (lookup)

Autocomplete-driven. Used as FK from `consumption`.

```sql
CREATE TABLE weather (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);
```

Autocomplete: `SELECT Id, Name FROM weather WHERE Name LIKE '<prefix>%' ORDER BY Name LIMIT N`

### 3.3 `consumption`

Per-trip record. Read from dashboard at start and end.

```sql
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
```

### 3.4 `source_category` (lookup)

Same autocomplete pattern as `weather`.

```sql
CREATE TABLE source_category (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);
```

### 3.5 `charger_type_enum`

PostgreSQL ENUM type.

```sql
CREATE TYPE charger_type_enum AS ENUM ('fast', 'slow');
```

### 3.6 `charging`

Per-charge-session record.

```sql
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
```

### 3.7 Indexes (recommendation)

For efficient time-range queries in `DisplayService`:

```sql
CREATE INDEX idx_consumption_vehicle_start ON consumption(VehicleId, Start);
CREATE INDEX idx_charging_vehicle_starttime ON charging(VehicleId, StartTime);
```

### 3.8 Constraints (NOT enforced in v1)

Application-level validation only — see §7 for details.

---

## 4. gRPC API

Proto package: `evgrpc`.

**Authentication:** All RPCs require a valid `Authorization: Bearer <JWT>` header. Requests without a valid token return `UNAUTHENTICATED` (gRPC code 16) before reaching the service handler. See §5 for token validation details.

### 4.1 Service: `VehicleService`

| RPC | Request | Response |
|---|---|---|
| `CreateVehicle` | `CreateVehicleRequest` | `Vehicle` |
| `GetVehicle` | `GetVehicleRequest { uuid id }` | `Vehicle` |
| `UpdateVehicle` | `UpdateVehicleRequest` | `Vehicle` |
| `DeleteVehicle` | `DeleteVehicleRequest { uuid id }` | `google.protobuf.Empty` |
| `ListVehicles` | `ListVehiclesRequest { int32 page_size, string page_token }` | `ListVehiclesResponse` |

`CreateVehicle` returns `ALREADY_EXISTS` if `LicensePlate` collides.

### 4.2 Service: `WeatherService`

| RPC | Request | Response |
|---|---|---|
| `CreateWeather` | `CreateWeatherRequest { string name }` | `Weather` |
| `SearchWeather` | `SearchWeatherRequest { string prefix, int32 limit }` | `SearchWeatherResponse { repeated Weather matches }` |

`SearchWeather`: `Name LIKE '<prefix>%' ORDER BY Name LIMIT N`.
`CreateWeather` returns `ALREADY_EXISTS` on UNIQUE collision.

### 4.3 Service: `SourceCategoryService`

Same shape as `WeatherService`:

| RPC | Request | Response |
|---|---|---|
| `CreateSourceCategory` | `CreateSourceCategoryRequest { string name }` | `SourceCategory` |
| `SearchSourceCategory` | `SearchSourceCategoryRequest { string prefix, int32 limit }` | `SearchSourceCategoryResponse { repeated SourceCategory matches }` |

### 4.4 Service: `ConsumptionService`

| RPC | Request | Response |
|---|---|---|
| `CreateConsumption` | `CreateConsumptionRequest` | `Consumption` |
| `GetConsumption` | `GetConsumptionRequest { uuid id }` | `Consumption` |
| `UpdateConsumption` | `UpdateConsumptionRequest` | `Consumption` |
| `DeleteConsumption` | `DeleteConsumptionRequest { uuid id }` | `google.protobuf.Empty` |
| `ListConsumptions` | `ListConsumptionsRequest { uuid vehicle_id, optional Timestamp start_after, optional Timestamp start_before, int32 page_size, string page_token }` | `ListConsumptionsResponse` |

### 4.5 Service: `ChargingService`

| RPC | Request | Response |
|---|---|---|
| `CreateCharging` | `CreateChargingRequest` | `Charging` |
| `GetCharging` | `GetChargingRequest { uuid id }` | `Charging` |
| `UpdateCharging` | `UpdateChargingRequest` | `Charging` |
| `DeleteCharging` | `DeleteChargingRequest { uuid id }` | `google.protobuf.Empty` |
| `ListChargings` | `ListChargingsRequest { uuid vehicle_id, optional Timestamp start_after, optional Timestamp start_before, optional ChargerType charger_type, optional uuid source_category_id, int32 page_size, string page_token }` | `ListChargingsResponse` |

`ChargerType` proto enum:

```
enum ChargerType {
  CHARGER_TYPE_UNSPECIFIED = 0;
  CHARGER_TYPE_FAST = 1;
  CHARGER_TYPE_SLOW = 2;
}
```

Stored as PostgreSQL `charger_type_enum` (`'fast'`, `'slow'`). Conversion at the storage layer.

### 4.6 Service: `DisplayService`

Filter parameters:

- `vehicle_id` — optional for most RPCs (omitted = aggregate across all vehicles); **required** for `GetVehicleCostSummary`.
- `start_time` / `end_time` — optional date range (inclusive). Omitted = no time filter.
- `year` / `month` — required for `GetAnnualReport` / `GetMonthlyReport` (calendar year / month).

| RPC | Required params | Returns |
|---|---|---|
| `GetVehicleCostSummary` | `vehicle_id` | Total cost, total kWh, avg 元/kWh, avg 元/km |
| `GetMonthlyReport` | `year`, `month`, optional `vehicle_id` | Total cost, total kWh, total km for that calendar month |
| `GetAnnualReport` | `year`, optional `vehicle_id` | Total cost, total kWh, total km for that calendar year |
| `GetCostByChargerType` | optional `vehicle_id`, optional time range | Per-charger-type breakdown (avg 元/kWh, total cost, total kWh) |
| `GetCostBySourceCategory` | optional `vehicle_id`, optional time range | Per-source-category breakdown |
| `GetConsumptionEfficiency` | optional `vehicle_id`, optional time range | km/kWh, kWh/100km |
| `GetRangeAccuracy` | optional `vehicle_id`, optional time range | Dashboard range estimate vs actual mileage difference (%) |
| `GetTemperatureConsumptionCorrelation` | optional `vehicle_id`, optional time range | kWh/100km bucketed by temperature range (<0℃, 0-10℃, 10-20℃, 20-30℃, >30℃) |

---

## 5. Authentication & Authorization

### 5.1 Role

evGRpc acts as an **OAuth 2.0 Resource Server**. It does **not** issue tokens — it only validates bearer tokens presented by clients. Token issuance, user registration, and login flows are handled by an external OAuth 2.0 / OIDC provider (e.g., Keycloak, Auth0, GitHub OAuth).

### 5.2 Token Format

- **JWT** (RFC 7519), signed with **RS256** (asymmetric; verification via JWKS public keys).
- Transmitted in `Authorization: Bearer <JWT>` header.

### 5.3 Validation Flow

Performed by a gRPC ServerInterceptor before each RPC:

1. Extract `Authorization` header from incoming metadata
2. If absent → `UNAUTHENTICATED`
3. Parse JWT header → fetch `kid` (key id)
4. Look up public key from **JWKS cache** (fetched from `OAUTH_JWKS_URL`, refreshed on TTL expiry or unknown-`kid` — see plan for cache strategy)
5. Verify RS256 signature with the public key
6. Validate standard claims:
   - `iss` equals `OAUTH_ISSUER_URL`
   - `aud` contains `OAUTH_AUDIENCE`
   - `exp` > now (and `nbf` < now if present)
7. On success: extract `sub` (subject) for audit logging (see §5.6); pass through to RPC handler
8. On failure: `UNAUTHENTICATED` (with no detail leak)

The interceptor must **fail-closed** — any error in token parsing, key lookup, signature verification, or claim validation returns `UNAUTHENTICATED`. No partial validation.

### 5.4 Configuration

> **JWKS URL is auto-derived from `oauth.issuer_url` via OIDC discovery at startup (see `2026-08-06-config-json-migration.md` §3.1).** `oauth.jwks_url` is no longer user-configurable.

| Env var | Purpose | Required |
|---|---|---|
| `OAUTH_ISSUER_URL` | Expected JWT `iss` claim | yes |
| `OAUTH_AUDIENCE` | Expected JWT `aud` claim | yes |
| `OAUTH_JWKS_URL` | JWKS endpoint URL (e.g., `https://idp.example.com/.well-known/jwks.json`) | yes |
| `OAUTH_JWKS_CACHE_TTL` | JWKS cache TTL in seconds | no (default `3600`) |

If any required env var is missing at startup, the server refuses to start (fail-fast).

### 5.5 Authorization

In v1, **any valid token can call any RPC**. No scope-based authorization, no role-based access control. The single auth check is "is this token valid?"

Per-RPC scope differentiation (e.g., `evgrpc:read` vs `evgrpc:write`) is deferred to v2.

The `sub` claim is **not** persisted in business tables in v1 — single-user assumed.

### 5.6 Logging

Structured logging via **spdlog** v1.x. stderr + stdout + (optional) rotating file sink — see Sinks below.

**Named loggers** (spdlog channels; all initialized to `LOG_LEVEL`):

| Logger | Owner | What it logs |
|---|---|---|
| `auth` | `evgrpc::Authenticate` (Task 9) | every RPC's auth outcome (pass/fail + reason category) |
| `service` | every gRPC service method (Tasks 10–14, 16–19) | per-RPC entry/exit with `req_id`, `subject`, `status_code`, `latency_ms` |
| `db` | `PgPool` (Task 4) + DB error mapping (Task 5) | pool acquire/release, query time, error category |
| `jwks` | `JwksCache` (Task 8) | cache miss, key rotation, refresh latency, parse failures |
| `server` | `main.cc` + signal handling (Task 15) | startup, config load result, shutdown, SIGHUP-level reload |

**Levels** (spdlog standard): `trace / debug / info / warn / error / critical`. Default `info`.

**Sinks**:

- **stdout** (color sink, TTY auto-detect): receives every level ≥ `LOG_LEVEL`. INFO/WARN/DEBUG/TRACE go here.
- **stderr** (color sink, TTY auto-detect): receives **only** `error` and `critical`. Lets log shippers split "operational noise" from "needs attention" along the conventional UNIX boundary.
- **rotating file sink** (only if `LOG_FILE` is set): receives every level ≥ `LOG_LEVEL`. Default rotation: 100 MB × 7 files. Path defaults to `${LOG_FILE}` — Docker users typically mount a tmpfs or volume here for graceful log rotation.

**Format** (default = text):

```
[2026-07-31 08:04:00.123 +0800] [info] [auth] method=CreateVehicle subject=alice reason=ok req_id=7a3f...
```

`%+` color flag is on by default for stderr/stdout when the destination is a TTY; off when piped to a file. JSON output is deferred to v2 — text is the canonical format for v1 (grep-friendly, sufficient for `journald` / fluentd / Loki parsing with `| awk` or `jq`).

**Conventions**:

- **Never log secrets**: no `Authorization` header values, no JWT tokens, no DB passwords, no PEM private keys, no JWKS responses verbatim.
- **Auth outcome log format** (on every RPC): one of
  - Pass: `method=<RPC> subject=<sub> reason=ok req_id=<uuid>`
  - Fail: `method=<RPC> subject=<unknown> reason=<category> req_id=<uuid>`
    where `<category>` is one of `missing_header`, `non_bearer`, `malformed`, `bad_signature`, `expired`, `unknown_kid`, `wrong_issuer`, `wrong_audience` (NOT the raw error message).
- **Service entry/exit log**: `service.info("req_id={} method={} subject={}", ...)` on entry; `service.info("req_id={} method={} status={} latency_ms={}", ...)` on exit (success or failure). Service handlers should call these via a small `RpcScope` RAII helper (introduced in Task 10) that times the handler and logs on destruction.
- **No log spam at INFO**: HTTP-cache refresh (JWKS) and pool acquire/release are `debug`, not `info`.

**Env vars** (read at `log::Init()`):

| Var | Default | Notes |
|---|---|---|
| `LOG_LEVEL` | `info` | applies to all sinks (per-sink overrides not exposed in v1) |
| `LOG_FORMAT` | `text` | text only in v1; `json` is reserved (rejected with warning, falls back to text) |
| `LOG_FILE` | empty | if set, enables the rotating file sink at this path |
| `LOG_FILE_MAX_SIZE_MB` | `100` | rotation size threshold |
| `LOG_FILE_MAX_FILES` | `7` | number of rotated files retained |

---

## 6. Service Boundary

6 independent gRPC services, each generated from its own `.proto` file:

```
proto/evgrpc/
  vehicle.proto             → VehicleService
  weather.proto             → WeatherService
  source_category.proto     → SourceCategoryService
  consumption.proto         → ConsumptionService
  charging.proto            → ChargingService
  display.proto             → DisplayService
```

C++ code organization (one directory per service):

```
src/services/
  vehicle_service.{h,cc}
  weather_service.{h,cc}
  source_category_service.{h,cc}
  consumption_service.{h,cc}
  charging_service.{h,cc}
  display_service.{h,cc}
```

All services register against a single `ServerBuilder` and listen on one port. The auth interceptor (§5) wraps every RPC regardless of which service handles it.

---

## 7. Error Handling

| Code | When |
|---|---|
| `UNAUTHENTICATED` (16) | Missing or invalid bearer token (§5) |
| `NOT_FOUND` (5) | Record ID does not exist |
| `ALREADY_EXISTS` (6) | UNIQUE constraint violation |
| `INVALID_ARGUMENT` (3) | Bad input (e.g., `EndPercent < BeginPercent`, invalid time range) |
| `INTERNAL` (13) | Database errors, unexpected exceptions |

Application-level validation runs **before** hitting PostgreSQL — so most business-rule violations surface as `INVALID_ARGUMENT` and don't leak to `INTERNAL`.

---

## 8. Testing Strategy

- **Unit tests** for business logic (validation rules, computation helpers).
- **Integration tests** using [`testcontainers-cpp`](https://github.com/testcontainers/testcontainers-cpp) to spin up ephemeral PostgreSQL per test run.
- **End-to-end tests** via a generated gRPC C++ client against a running test server (in-process server fixture).
- **Auth tests** (new): test interceptor with valid JWT, expired JWT, wrong issuer, wrong audience, invalid signature, missing token, malformed header.
- **No load testing** in v1.

---

## 9. Deployment

### Build

- CMake (`cmake -G Ninja ..`)
- Ninja (`ninja`)
- Dependencies (decision in plan):
  - `grpc++`
  - `protobuf`
  - `libpqxx`
  - `nlohmann/json`
  - JWT library (e.g., `jwt-cpp`)
  - HTTP client for JWKS (e.g., `libcurl` or `cpr`)
  - `gtest` (testing)

### Docker

Multi-stage `Dockerfile`:

- **Stage 1 (`builder`)**: base image with `gcc` + `cmake` + `ninja` + `libpqxx-dev` + `libgrpc++-dev` + `libprotobuf-dev` + `libcurl-dev`. Runs CMake configure + Ninja build.
- **Stage 2 (`runtime`)**: minimal base with `libpq5` + `libgrpc++1` + runtime libs. Copies built binary. Sets `ENTRYPOINT`.

### Runtime Config

> **Superseded by [`2026-08-06-config-json-migration.md`](./2026-08-06-config-json-migration.md) §2 — config.json is the only config source in v2.** The env-var table below is retained for historical reference only.

Environment variables consumed at startup:

| Var | Purpose | Required |
|---|---|---|
| `DATABASE_URL` | PostgreSQL connection string | yes |
| `GRPC_PORT` | gRPC listen port | no (default `50051`) |
| `OAUTH_ISSUER_URL` | Expected JWT `iss` claim | yes |
| `OAUTH_AUDIENCE` | Expected JWT `aud` claim | yes |
| `OAUTH_JWKS_URL` | JWKS endpoint URL | yes |
| `OAUTH_JWKS_CACHE_TTL` | JWKS cache TTL seconds | no (default `3600`) |

Server refuses to start if any required env var is missing.

---

## 10. Open Questions (for Implementation Plan)

- Exact `FetchContent` vs `vcpkg` vs `apt` for dependencies
- CMake target structure (single library, multiple libraries, monorepo)
- Whether to add Prometheus metrics endpoint (likely out for v1)
- Specific index set beyond `idx_consumption_vehicle_start` and `idx_charging_vehicle_starttime`
- **JWKS HTTP client choice** (`libcurl` vs `cpr` vs other)
- **JWKS cache eviction strategy** (TTL + key rotation handling)
- **JWT library choice** (`jwt-cpp` vs writing a minimal RS256 verifier)
- **Test JWT generation strategy** (mock IdP via testcontainers vs static test RSA keys)

**Logging decision (closed 2026-07-31, Task 9.5):** `spdlog` v1.x. See §5.6.
**Log format decision (closed 2026-07-31):** structured text default, JSON deferred to v2.

---

## 11. Out of Scope (Explicit)

- **nginx reverse proxy** — v1 binary listens directly; nginx added in v2.
- **OAuth 2.0 server / token issuance** — handled by external IdP (Keycloak / Auth0 / any OIDC).
- **Scope-based authorization** — any valid token can call any RPC in v1.
- **Multi-user isolation** — no `user_id` columns; single-user assumed.
- **Multi-currency** — RMB only. Add `Currency` column later if needed.
- **External weather API** — manual entry only with autocomplete.
- **Time-of-use pricing** — no peak/valley tariff tracking.
- **Mobile / web UI** — gRPC only.
- **Backup / archival** — handled by PostgreSQL ops.