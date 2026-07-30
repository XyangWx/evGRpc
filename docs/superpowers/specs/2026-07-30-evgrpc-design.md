# evGRpc — Electric Vehicle Electricity Cost Tracking Service

**Date:** 2026-07-30
**Status:** Design — pending user approval
**Repo:** `/workspace/repositories/evGRpc`

---

## 1. Goals & Scope

### Goal

Build a gRPC service in C++ that **records and analyzes electricity costs for electric vehicles**. All data entry is manual. Analysis surfaces per-vehicle cost, charging efficiency, and weather/temperature correlation.

### In Scope (v1)

- C++ gRPC server (`grpc++`)
- PostgreSQL backend (external, connection-only)
- Dockerfile packaging the gRPC service
- Manual data entry via gRPC RPCs
- 5 functional modules: vehicle, consumption, weather, charging, display
- Multi-stage Docker build (CMake + Ninja)

### Out of Scope (v1)

- nginx reverse proxy — **deferred to v2**
- Authentication / multi-tenancy (single-user internal service)
- External weather API (manual entry with autocomplete)
- Currency conversion (RMB only)
- Web UI (gRPC clients are CLI / custom)
- Multi-user, multi-tenant
- Mobile / web clients

---

## 2. Architecture Overview

```
┌──────────────────┐
│  gRPC Client(s)  │
└────────┬─────────┘
         │ HTTP/2 (gRPC)
         ▼
┌──────────────────────────────┐
│  Docker Container            │
│  ┌────────────────────────┐  │
│  │  C++ gRPC server       │  │
│  │  (grpc++ + libpqxx)    │  │
│  └────────┬───────────────┘  │
└───────────┼──────────────────┘
            │ PostgreSQL protocol
            ▼
   ┌─────────────────┐
   │  PostgreSQL     │  (external, already deployed)
   └─────────────────┘
```

- **Single binary** runs all 6 gRPC services on one port (default `:50051`).
- **PostgreSQL** is external — connection via `DATABASE_URL` env var.
- **No internal state** — every request reads from / writes to PostgreSQL.
- **No caching layer** — analyses query PostgreSQL directly with aggregation.

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
  BeginPercent        INT NOT NULL,           -- 开始电量百分比
  EndPercent          INT NOT NULL,           -- 结束电量百分比
  BeginMileage        INT NOT NULL,           -- 起始里程 (km)
  EndMileage          INT NOT NULL,           -- 结束里程 (km)
  BeginRange          INT NOT NULL,           -- 开始续航里程 (km)
  EndRange            INT NOT NULL,           -- 结束续航里程 (km)
  HighestTemperature  DECIMAL(4,1) NOT NULL,  -- 最高气温 (℃)
  LowestTemperature   DECIMAL(4,1) NOT NULL,  -- 最低气温 (℃)
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

PostgreSQL ENUM type. Hard-coded values; extend via `ALTER TYPE` if needed.

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
  ElectricityUnitPrice  DECIMAL(4,2)  NOT NULL,  -- 元/kWh
  ServiceFee            DECIMAL(5,2),            -- 元 (nullable)
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

(Exact index list decided in implementation plan.)

### 3.8 Constraints (NOT enforced in v1)

Application-level validation only — to keep insertion forgiving during data entry:

- `End > Start` (charging & consumption)
- `EndPercent > BeginPercent`
- `EndMileage >= BeginMileage`
- `EndRange >= BeginRange`
- `HighestTemperature >= LowestTemperature`
- `Cost >= 0`, `KwhCharged > 0`, `ElectricityUnitPrice > 0`, `ServiceFee >= 0` (if non-null)

These may be promoted to DB CHECK constraints later if data quality warrants it.

---

## 4. gRPC API

Proto package: `evgrpc`.

### 4.1 Service: `VehicleService`

| RPC | Request | Response |
|---|---|---|
| `CreateVehicle` | `CreateVehicleRequest` | `Vehicle` |
| `GetVehicle` | `GetVehicleRequest { uuid id }` | `Vehicle` |
| `UpdateVehicle` | `UpdateVehicleRequest` | `Vehicle` |
| `DeleteVehicle` | `DeleteVehicleRequest { uuid id }` | `google.protobuf.Empty` |
| `ListVehicles` | `ListVehiclesRequest { int32 page_size, string page_token }` | `ListVehiclesResponse` |

`CreateVehicle` returns `ALREADY_EXISTS` if `LicensePlate` collides with existing vehicle.

### 4.2 Service: `WeatherService`

| RPC | Request | Response |
|---|---|---|
| `CreateWeather` | `CreateWeatherRequest { string name }` | `Weather` |
| `SearchWeather` | `SearchWeatherRequest { string prefix, int32 limit }` | `SearchWeatherResponse { repeated Weather matches }` |

`SearchWeather` implements prefix match: `Name LIKE '<prefix>%' ORDER BY Name LIMIT N`.
`CreateWeather` returns `ALREADY_EXISTS` if `Name` collides with existing entry (UNIQUE constraint).

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

`ChargerType` is a proto enum with three values (per standard protobuf convention, includes `UNSPECIFIED`):

```
enum ChargerType {
  CHARGER_TYPE_UNSPECIFIED = 0;
  CHARGER_TYPE_FAST = 1;
  CHARGER_TYPE_SLOW = 2;
}
```

Stored as PostgreSQL `charger_type_enum` (`'fast'`, `'slow'`). Conversion happens at the storage layer.

### 4.6 Service: `DisplayService`

All analysis RPCs. Filter parameters:

- `vehicle_id` — optional for most RPCs (omitted = aggregate across all vehicles); **required** for `GetVehicleCostSummary` (per-vehicle).
- `start_time` / `end_time` — optional date range (inclusive). Omitted = no time filter.
- `year` / `month` — required for `GetAnnualReport` / `GetMonthlyReport` (calendar year / month, integer).

| RPC | Required params | Returns |
|---|---|---|
| `GetVehicleCostSummary` | `vehicle_id` | Total cost, total kWh, avg 元/kWh, avg 元/km |
| `GetMonthlyReport` | `year`, `month`, optional `vehicle_id` | Total cost, total kWh, total km for that calendar month |
| `GetAnnualReport` | `year`, optional `vehicle_id` | Total cost, total kWh, total km for that calendar year |
| `GetCostByChargerType` | optional `vehicle_id`, optional time range | Per-charger-type breakdown (avg 元/kWh, total cost, total kWh) |
| `GetCostBySourceCategory` | optional `vehicle_id`, optional time range | Per-source-category breakdown |
| `GetConsumptionEfficiency` | optional `vehicle_id`, optional time range | km/kWh, kWh/100km |
| `GetRangeAccuracy` | optional `vehicle_id`, optional time range | Dashboard range estimate vs actual mileage difference (%) |
| `GetTemperatureConsumptionCorrelation` | optional `vehicle_id`, optional time range | kWh/100km bucketed by temperature range (e.g., <0℃, 0-10℃, 10-20℃, 20-30℃, >30℃) |

Detailed response message shapes are specified in `proto/evgrpc/display.proto` (during plan / implementation).

---

## 5. Service Boundary

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

All services register against a single `ServerBuilder` and listen on one port.

---

## 6. Error Handling

Standard gRPC status codes:

| Code | When |
|---|---|
| `NOT_FOUND` | Record ID does not exist (Get/Update/Delete on missing row) |
| `ALREADY_EXISTS` | UNIQUE constraint violation (LicensePlate, weather.Name, source_category.Name) |
| `INVALID_ARGUMENT` | Bad input (e.g., `EndPercent < BeginPercent`, invalid time range) |
| `INTERNAL` | Database errors, unexpected exceptions |

Application-level validation runs **before** hitting PostgreSQL — so most business-rule violations surface as `INVALID_ARGUMENT` and don't leak to `INTERNAL`.

---

## 7. Testing Strategy

- **Unit tests** for business logic (validation rules, computation helpers).
- **Integration tests** using [`testcontainers-cpp`](https://github.com/testcontainers/testcontainers-cpp) to spin up ephemeral PostgreSQL per test run.
- **End-to-end tests** via a generated gRPC C++ client against a running test server (in-process server fixture).
- **No load testing** in v1.

---

## 8. Deployment

### Build

- CMake (`cmake -G Ninja ..`)
- Ninja (`ninja`)
- Dependencies fetched via CMake `FetchContent` or system package manager (decision in plan):
  - `grpc++`
  - `protobuf`
  - `libpqxx`
  - `nlohmann/json`
  - `gtest` (testing)

### Docker

Multi-stage `Dockerfile`:

- **Stage 1 (`builder`)**: base image with `gcc` + `cmake` + `ninja` + `libpqxx-dev` + `libgrpc++-dev` + `libprotobuf-dev`. Runs CMake configure + Ninja build.
- **Stage 2 (`runtime`)**: minimal base with `libpq5` + `libgrpc++1` + runtime libs. Copies built binary. Sets `ENTRYPOINT`.

### Runtime Config

Environment variables consumed at startup:

- `DATABASE_URL` — PostgreSQL connection string (required)
- `GRPC_PORT` — listen port (default `50051`)

---

## 9. Open Questions (for Implementation Plan)

These are intentionally deferred to the plan document:

- Exact `FetchContent` vs `vcpkg` vs `apt` for dependencies
- CMake target structure (single library, multiple libraries, monorepo)
- Logging library choice (e.g., `spdlog`, `glog`, raw `std::cerr`)
- Whether to add Prometheus metrics endpoint (likely out for v1)
- Specific index set beyond `idx_consumption_vehicle_start` and `idx_charging_vehicle_starttime`

---

## 10. Out of Scope (Explicit)

- **nginx reverse proxy** — v1 binary listens directly; nginx added in v2.
- **Authentication** — internal single-user service, no auth layer.
- **Multi-currency** — RMB only. Add `Currency` column later if needed.
- **External weather API** — manual entry only with autocomplete.
- **Time-of-use pricing** — no peak/valley tariff tracking (could be added later via `TariffPeriod` column).
- **Mobile / web UI** — gRPC only; no frontend in this project.
- **Backup / archival** — handled by PostgreSQL ops, not in this service.