# DisplayService Charging Reports (Year / Month / Day)

- **Date:** 2026-08-13
- **Status:** Design (awaiting review)
- **Replaces:** none
- **Supersedes:** none

## 1. Background

`DisplayService` already has `GetMonthlyReport` and `GetAnnualReport` that
aggregate **mixed** data: `total_cost` and `total_kwh` come from `charging`,
but `total_km` comes from `consumption` (`SUM(EndMileage - BeginMileage)`).
The mix-in makes the existing `PeriodReport` ill-suited for charging-only
dashboards, and there is **no daily report at any granularity** — only
monthly and annual.

We want three charging-only RPCs at year / month / day granularity,
with a new `ChargingReport` message parallel to (but distinct from)
`PeriodReport`. The existing RPCs and `PeriodReport` stay untouched —
they continue to serve the "mixed monthly mileage" use case if anyone
relies on it. New RPCs establish a cleaner pattern: charging-only data,
TZ-aware day boundaries, no-data = zero rows (not INTERNAL).

## 2. Goals

1. Add **3 new RPCs** to `DisplayService`:
   - `GetDailyChargingReport(year, month, day, vehicle_id) → ChargingReport`
   - `GetMonthlyChargingReport(year, month, vehicle_id) → ChargingReport`
   - `GetAnnualChargingReport(year, vehicle_id) → ChargingReport`
2. Add a new proto message `ChargingReport` with **charging-only fields**:
   `year`, `month`, `day`, `total_cost`, `total_kwh`, `count`, `vehicle_id`.
3. **TZ-aware date boundaries** for all three RPCs. The day/month/year
   a charging event falls into is determined by converting its UTC
   `StartTime` to the **current PG session timezone** at query time
   (`current_setting('TIMEZONE')`). The session TZ is set by libpq from
   the `TZ` environment variable when the server starts.
4. **No-data → OK with zeros.** Any date (including future dates and
   dates with zero charging events) returns `OK` with
   `total_cost=0, total_kwh=0, count=0`. The existing
   `GetMonthlyReport`/`GetAnnualReport` "no aggregate row → INTERNAL"
   quirk is **not** propagated to the new RPCs.
5. ~12 new integration tests covering happy path, empty, vehicle filter,
   invalid arguments, and a TZ-boundary case.

## 3. Non-Goals

- Modifying or deprecating the existing `PeriodReport`,
  `GetMonthlyReport`, or `GetAnnualReport`. They keep their current
  UTC-aligned behavior.
- Per-charger-type or per-source-category breakdown within a charging
  report. `GetCostByChargerType` and `GetCostBySourceCategory` already
  cover those dimensions.
- Adding a `count` of "days with at least one charge" (i.e. how many
  distinct days had activity within a month). YAGNI; the client can
  iterate over daily reports if it needs this.
- TZ configuration via `config.json`. The `TZ` env var is the standard
  Linux convention and is sufficient.
- Range queries like "give me daily reports for an arbitrary date range".
  A separate spec if needed.
- Cache layer. Reports aggregate from PG on each call; existing RPCs do
  the same.

## 4. Architecture

```
DisplayService
 ├─ GetVehicleCostSummary         (existing, mixed date range)
 ├─ GetMonthlyReport              (existing, mixed; UTC)
 ├─ GetAnnualReport               (existing, mixed; UTC)
 ├─ GetCostByChargerType          (existing)
 ├─ GetCostBySourceCategory       (existing)
 ├─ GetConsumptionEfficiency      (existing)
 ├─ GetRangeAccuracy              (existing)
 ├─ GetTemperatureConsumptionCorrelation  (existing)
 ├─ GetDailyChargingReport        (NEW — charging only, TZ-aware)
 ├─ GetMonthlyChargingReport      (NEW — charging only, TZ-aware)
 └─ GetAnnualChargingReport       (NEW — charging only, TZ-aware)
```

The new RPCs share an aggregation SQL pattern:

```
SUM/COUNT aggregates FROM charging c
WHERE date_part_filters on (c.StartTime AT TIME ZONE current_setting('TIMEZONE'))
  AND optional vehicle_id filter
COALESCE(SUM(...), 0) so empty result → all zeros (not null)
COUNT(*) returns ≥0 unconditionally
```

## 5. Proto Changes

### 5.1 New messages (`proto/evgrpc/display.proto`)

```protobuf
// Charging-only report. Parallel to PeriodReport but reads only the
// charging table (no consumption mix-in) and adds a charging-event
// count. day=0 for annual/monthly; month=0 for annual — matches
// PeriodReport's convention so a single decode path works for all
// three granularities.
message ChargingReport {
  int32 year = 1;
  int32 month = 2;        // 0 = annual; 1-12 = monthly or daily
  int32 day = 3;          // 0 = annual/monthly; 1-31 = daily
  double total_cost = 4;
  double total_kwh = 5;
  int32 count = 6;
  string vehicle_id = 7;  // empty = all vehicles
}

message GetDailyChargingReportRequest {
  int32 year = 1;
  int32 month = 2;
  int32 day = 3;
  string vehicle_id = 4;  // optional
}

message GetMonthlyChargingReportRequest {
  int32 year = 1;
  int32 month = 2;
  string vehicle_id = 3;  // optional
}

message GetAnnualChargingReportRequest {
  int32 year = 1;
  string vehicle_id = 2;  // optional
}
```

### 5.2 Service definition

In the `service DisplayService` block:

```protobuf
rpc GetDailyChargingReport(GetDailyChargingReportRequest) returns (ChargingReport);
rpc GetMonthlyChargingReport(GetMonthlyChargingReportRequest) returns (ChargingReport);
rpc GetAnnualChargingReport(GetAnnualChargingReportRequest) returns (ChargingReport);
```

## 6. SQL Design

### 6.1 TZ-aware date math

PG stores `charging.StartTime` as `TIMESTAMP` (interpreted by libpq as
the server's session TZ at insert time, then compared in UTC for
`timestamptz` semantics). For date-boundary grouping we need:

```sql
(c.StartTime AT TIME ZONE current_setting('TIMEZONE'))::date
```

`current_setting('TIMEZONE')` returns the session-level TZ, set by libpq
when the connection is established from the `TZ` env var. Default = `UTC`
(PG initdb default).

### 6.2 SQL per RPC

**GetDailyChargingReport** (cleanest — single date equality):

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE (c.StartTime AT TIME ZONE current_setting('TIMEZONE'))::date
      = make_date($1, $2, $3)
  AND (length($4) = 0 OR c.VehicleId::text = $4);
```

**GetMonthlyChargingReport**:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE EXTRACT(YEAR  FROM (c.StartTime AT TIME ZONE current_setting('TIMEZONE'))) = $1
  AND EXTRACT(MONTH FROM (c.StartTime AT TIME ZONE current_setting('TIMEZONE'))) = $2
  AND (length($3) = 0 OR c.VehicleId::text = $3);
```

**GetAnnualChargingReport**:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE EXTRACT(YEAR FROM (c.StartTime AT TIME ZONE current_setting('TIMEZONE'))) = $1
  AND (length($2) = 0 OR c.VehicleId::text = $2);
```

### 6.3 Notes

- **`COUNT(*)` is unconditional** — it returns 0 on empty match, not
  NULL. No `COALESCE` needed.
- **`COALESCE(SUM(col), 0)`** is required because `SUM` over zero rows
  returns NULL, not 0. Without this the empty-result row would have
  NULLs in `total_cost`/`total_kwh`.
- **No precursor `EXISTS` pre-check.** The existing
  `GetMonthlyReport`/`GetAnnualReport` have an "EXISTS pre-check fires
  INTERNAL when zero rows match" pattern that causes
  `grpc::StatusCode::INTERNAL` to be returned when the requested period
  has no data. We **deliberately do not replicate this** — new RPCs
  return OK with zeros (see §2.4).
- **`length($N) = 0`** for the optional `vehicle_id` filter, mirroring
  the existing pattern from `GetMonthlyReport`. SQL is built up by
  appending `AND c.VehicleId = $N::text` only when the field is
  non-empty, keeping the params object simple.

## 7. Validation

| RPC | Field | Rule | Error |
|---|---|---|---|
| `GetDailyChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |
| | `month` | `1..=12` | `INVALID_ARGUMENT` |
| | `day` | `1..=31` | `INVALID_ARGUMENT` |
| `GetMonthlyChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |
| | `month` | `1..=12` | `INVALID_ARGUMENT` |
| `GetAnnualChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |

**Day-vs-month validity (e.g. Feb 30):** NOT validated server-side.
`make_date($1, $2, $3)` with `day=30, month=2` would raise
`PG SQLSTATE 22008 (date_out_of_range)`. Since our validator rejects
`day > 31` upfront, this never reaches PG — but if someone refactors the
validator and forgets, PG's error becomes a 500. Defense: validator
must always reject `day > 31` (already in spec). Client is responsible
for Feb 30 / Apr 31 / etc.; such queries return **OK with zeros** under
the no-data policy.

## 8. Error Semantics

| Scenario | Response |
|---|---|
| Valid args, data exists | `OK` + `ChargingReport` with real aggregates |
| Valid args, no data for that period | `OK` + `ChargingReport{total_cost=0, total_kwh=0, count=0}` |
| Valid args, far-future date (e.g. year=2099) | `OK` + zero `ChargingReport` |
| Invalid args (year<1900, month out of range, day out of range) | `INVALID_ARGUMENT` |
| Day is invalid for the month (e.g. Feb 30) | **OK + zero** (validator catches day<=31; client responsible) |
| DB transient error | `INTERNAL` (via existing `ToGrpcStatus` mapping) |

This deliberately differs from `GetMonthlyReport`/`GetAnnualReport`
which return `INTERNAL "no aggregate row"` for empty result sets. The
new RPCs establish a cleaner pattern.

## 9. Tests (`tests/integration/display_service_test.cc`)

All tests run in the shared PG (per existing `ServiceITBase` fixture).
PG session TZ is whatever libpq sets from the test environment's `TZ`
var. CI / dev machine default: assume `UTC` (no `TZ` env → PG default).
A single TZ-boundary test sets session TZ explicitly via
`SET TIME ZONE 'Asia/Shanghai'` to verify cross-midnight grouping.

### 9.1 Common assertions

For "happy path with multiple rows":
- Create 3 charging events for one vehicle on the **same UTC day** with
  distinct `StartTime` values spanning that day.
- Call the relevant RPC for that vehicle's date.
- Assert `total_cost == sum(Cost)`, `total_kwh == sum(KwhCharged)`,
  `count == 3`, year/month/day echoed back correctly.

For "empty / no data":
- TruncateAll clears tables; create no rows.
- Call the RPC for an arbitrary past date.
- Assert `OK`, `total_cost==0`, `total_kwh==0`, `count==0`.

For "vehicle filter":
- Create rows for vehicle A and vehicle B.
- Call with `vehicle_id = A.id`.
- Assert only A's rows aggregate.

### 9.2 Test list

| TEST_F | RPC | Scenario |
|---|---|---|
| `GetDailyChargingReport_HappyPath_MultipleRows` | daily | 3 events same day → aggregates |
| `GetDailyChargingReport_Empty` | daily | no data → OK + zeros |
| `GetDailyChargingReport_VehicleFilter` | daily | filter excludes other vehicles |
| `GetDailyChargingReport_YearBelow1900_InvalidArgument` | daily | year=1899 → INVALID_ARGUMENT |
| `GetDailyChargingReport_MonthOutOfRange_InvalidArgument` | daily | month=0 / month=13 → INVALID_ARGUMENT |
| `GetDailyChargingReport_DayOutOfRange_InvalidArgument` | daily | day=0 / day=32 → INVALID_ARGUMENT |
| `GetDailyChargingReport_Feb30_OkZero` | daily | day=30, month=2 → OK + zeros |
| `GetDailyChargingReport_TzBoundary_MidnightRollsToNextDay` | daily | charging event at 23:30 UTC → grouped in Asia/Shanghai next day |
| `GetMonthlyChargingReport_HappyPath_MultipleRows` | monthly | events spanning 3 days in same month → aggregates |
| `GetMonthlyChargingReport_Empty` | monthly | no data → OK + zeros |
| `GetMonthlyChargingReport_VehicleFilter` | monthly | filter |
| `GetMonthlyChargingReport_YearBelow1900_InvalidArgument` | monthly | year=1899 |
| `GetMonthlyChargingReport_MonthOutOfRange_InvalidArgument` | monthly | month=0 / 13 |
| `GetAnnualChargingReport_HappyPath_MultipleRows` | annual | events across 2 months → aggregates |
| `GetAnnualChargingReport_Empty` | annual | no data → OK + zeros |
| `GetAnnualChargingReport_VehicleFilter` | annual | filter |
| `GetAnnualChargingReport_YearBelow1900_InvalidArgument` | annual | year=1899 |

**~17 new TEST_F** (initial estimate of ~12 was conservative — TZ-boundary
test pulls the count up). All under 60 s cumulative (each is a single
aggregate query, sub-second).

### 9.3 TZ-boundary test specifics

```cpp
TEST_F(DisplayServiceIT, GetDailyChargingReport_TzBoundary_MidnightRollsToNextDay) {
  // Establish a known TZ for this test
  pqxx::work tx(*pool_->acquire());
  tx.exec("SET TIME ZONE 'Asia/Shanghai'");
  tx.commit();

  // Create a charging event at 2026-08-12T20:00:00Z (UTC),
  // which is 2026-08-13T04:00:00+08:00 (Shanghai) — i.e. it
  // should be counted in the 2026-08-13 Shanghai daily report.
  ...

  // Query: GetDailyChargingReport(2026, 8, 13) should include it.
  // Query: GetDailyChargingReport(2026, 8, 12) should NOT include it.
}
```

Tests run in serial within a single PG session (shared fixture), so
`SET TIME ZONE` inside one test affects subsequent tests **unless reset**.
The test fixture (`ServiceITBase::SetUp`) must `RESET TIME ZONE` (or
re-establish UTC) at start of every test, or this single TZ test must
be the last one. **Implementation: the test fixture resets TZ to UTC
in `SetUp()`** (cheap — one statement per test).

## 10. Known Limitations / Follow-ups

1. **Old RPCs stay UTC-aligned.** `GetMonthlyReport` / `GetAnnualReport`
   continue to use `EXTRACT(YEAR/MONTH FROM StartTime)` without `AT TIME
   ZONE`, so they group by UTC date. The new RPCs group by local TZ
   date. For a charging event at UTC midnight in a non-UTC TZ, the two
   RPC families can disagree. This is a deliberate trade-off chosen in
   design (option A: "add new RPCs, don't replace existing") — fixing
   the old RPCs is a breaking change that can be done as a separate spec
   when there's user demand.

2. **No `config.json` TZ field.** TZ is controlled exclusively via the
   `TZ` environment variable on the server process. Operators must
   set `TZ=Asia/Shanghai` (or similar) on container start. README and
   any deployment docs should mention this.

3. **No pagination.** Each report returns aggregates over a single
   period — no row-level pagination. For row-level charging list with
   day filters, the existing `ListChargings` `start_after` /
   `start_before` filters can be used (see MEMORY: range filter is
   untested and could be added as a follow-up spec).

4. **No aggregation caching.** Reports run `SUM`/`COUNT` from PG on
   every call. Acceptable for now (charging table is small, a single
   user's data).

## 11. Files Touched

```
proto/evgrpc/display.proto                                       [MOD] +ChargingReport, +3 requests, +3 rpc
src/proto/evgrpc/display.pb.cc + display.pb.h                     [AUTO] protoc regen
src/proto/evgrpc/display_grpc.pb.cc + display_grpc.pb.h           [AUTO] protoc regen
src/services/display_service.cc                                  [MOD] +3 handler impls (~150 lines)
tests/integration/display_service_test.cc                        [MOD] +~17 TEST_F
tests/integration/service_test_fixtures.h / .cc                   [MOD] +RESET TIME ZONE in ServiceITBase::SetUp
docs/superpowers/specs/2026-08-13-display-charging-reports-design.md  [NEW] this file
```

## 12. Out of Scope, but Mentioned

- README updates documenting the new RPCs and `TZ` env var convention
  (covered by this spec's implementation plan, not the spec itself).
- Updating MEMORY.md with "TZ-aware ChargingReport" entry once shipped.