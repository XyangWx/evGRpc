# DisplayService Charging Reports (Year / Month / Day) — v2

- **Date:** 2026-08-13
- **Status:** Design (v2, awaiting re-review after first-review issues)
- **Replaces:** none
- **Supersedes:** v1 of this spec (rejected: SQL correctness issue with TIMESTAMP column)

## 0. Revision History

- **v1 → v2:** Spec reviewer found that `charging.StartTime` is `TIMESTAMP`
  (not `TIMESTAMPTZ`), making TZ-aware SQL a no-op. Added schema
  migration as a precondition. Simplified SQL since storage is now
  correct. Other reviewer issues (Feb 30 contradiction, libpq
  attribution, SQL construction inconsistency, fixture-wide TZ
  leakage) addressed.

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
TZ-aware day boundaries (via TIMESTAMPTZ storage), no-data = zero rows
(not INTERNAL).

### 1.1 Why a schema migration is required (reviewer feedback)

The current schema (`sql/001_initial.sql`) declares:

```sql
CREATE TABLE charging (
  ...
  StartTime  TIMESTAMP NOT NULL,    -- bare TIMESTAMP, no TZ info
  EndTime    TIMESTAMP NOT NULL,
  ...
);
```

`TIMESTAMP` (without `TZ`) is **wall-clock time with no TZ metadata**.
The application layer today casts the input to `timestamptz` at insert
time (`src/services/charging_service.cc:210`: `$3::timestamptz`), which
makes PG **convert the UTC instant to the session's local time and strip
the TZ info**, storing a bare literal. This means:

1. The same UTC instant can be stored as different literals depending on
   the **session TZ at INSERT time** (e.g. `20:00:00` in UTC session,
   `04:00:00` next day in Shanghai session).
2. Applying `AT TIME ZONE current_setting('TIMEZONE')` at query time to
   such a column **doesn't shift anything** — PG treats the stored
   literal as if it were in the requested TZ and converts to UTC, which
   is exactly the inverse of the strip at insert. Net: no-op.
3. There is no way to make TZ-aware grouping correct without either
   (a) migrating the column to `TIMESTAMPTZ` (preserves the UTC instant
   unambiguously), or (b) reinterpreting all historical data.

We choose (a) — see §6 below. The migration is idempotent (checks
column type before altering) so existing dev/test databases pick it up
automatically; the 001_initial.sql is also updated so new installations
get TIMESTAMPTZ from the start.

## 2. Goals

1. **Schema migration (precondition)** — migrate
   `charging.StartTime` and `charging.EndTime` from `TIMESTAMP` to
   `TIMESTAMPTZ`. Existing data interpreted as UTC. Migration is
   idempotent. See §6.
2. Add **3 new RPCs** to `DisplayService`:
   - `GetDailyChargingReport(year, month, day, vehicle_id) → ChargingReport`
   - `GetMonthlyChargingReport(year, month, vehicle_id) → ChargingReport`
   - `GetAnnualChargingReport(year, vehicle_id) → ChargingReport`
3. Add a new proto message `ChargingReport` with **charging-only fields**:
   `year`, `month`, `day`, `total_cost`, `total_kwh`, `count`, `vehicle_id`.
4. **TZ-aware date boundaries** for all three RPCs. The session TZ comes
   from PG's session config (postmaster `TZ` env or `postgresql.conf`
   `timezone =`), not from the application process. Day/month/year
   grouping is via `EXTRACT(YEAR/MONTH FROM c.StartTime)` /
   `c.StartTime::date`, both of which use session TZ for timestamptz.
5. **No-data → OK with zeros.** Any date (including future dates and
   dates with zero charging events) returns `OK` with
   `total_cost=0, total_kwh=0, count=0`. The existing
   `GetMonthlyReport`/`GetAnnualReport` "no aggregate row → INTERNAL"
   quirk is **not** propagated to the new RPCs.
6. ~17 new integration tests covering happy path, empty, vehicle filter,
   invalid arguments (incl. day-vs-month), and a TZ-boundary case.

## 3. Non-Goals

- Modifying or deprecating the existing `PeriodReport`,
  `GetMonthlyReport`, or `GetAnnualReport`. **RPC signatures stay
  unchanged**, but the charging-side grouping **becomes
  session-TZ-aligned** after the migration (because `charging.StartTime`
  is now TIMESTAMPTZ). This is a **behavior change**, not a non-change
  — see §10.1 for the implications (old RPCs mix charging session-TZ
  with consumption UTC, an inconsistency amplified by the migration).
- Migrating `consumption.Start` / `consumption.EndTime` to TIMESTAMPTZ.
  This is a **separate spec** because it affects the consumption
  service's parse/insert path and possibly the existing
  `ParseTimestamp` helper. The old `GetMonthlyReport`/`GetAnnualReport`
  will read `consumption` (still TIMESTAMP, UTC-aligned via `EXTRACT`)
  and `charging` (TIMESTAMPTZ, session-TZ-aligned) from the same RPC
  — a known TZ inconsistency, see §10.
- Per-charger-type or per-source-category breakdown within a charging
  report. `GetCostByChargerType` and `GetCostBySourceCategory` already
  cover those dimensions.
- Adding a `count` of "days with at least one charge" (i.e. how many
  distinct days had activity within a month). YAGNI; the client can
  iterate over daily reports if it needs this.
- TZ configuration via `config.json`. PG's session TZ (from
  postmaster env or `postgresql.conf`) is the standard convention and
  is sufficient.
- Range queries like "give me daily reports for an arbitrary date range".
  A separate spec if needed.
- Cache layer. Reports aggregate from PG on each call; existing RPCs do
  the same.
- Cleaning up the now-redundant `::timestamptz` casts in
  `src/services/charging_service.cc` (lines 210, 328, 408-414 etc.).
  After the column migration these casts are no-ops but not harmful.
  Separate cleanup spec if desired.

## 4. Architecture

```
DisplayService
 ├─ GetVehicleCostSummary         (existing)
 ├─ GetMonthlyReport              (existing — charging side now session-TZ,
 │                                  consumption side still UTC after this spec)
 ├─ GetAnnualReport               (existing — same TZ split as Monthly)
 ├─ GetCostByChargerType          (existing)
 ├─ GetCostBySourceCategory       (existing)
 ├─ GetConsumptionEfficiency      (existing)
 ├─ GetRangeAccuracy              (existing)
 ├─ GetTemperatureConsumptionCorrelation  (existing)
 ├─ GetDailyChargingReport        (NEW — charging only, session-TZ)
 ├─ GetMonthlyChargingReport      (NEW — charging only, session-TZ)
 └─ GetAnnualChargingReport       (NEW — charging only, session-TZ)
```

Schema after migration:

```
charging
 ├─ Id                UUID PRIMARY KEY
 ├─ VehicleId         UUID NOT NULL REFERENCES vehicle(Id)
 ├─ StartTime         TIMESTAMPTZ NOT NULL    -- migrated from TIMESTAMP
 ├─ EndTime           TIMESTAMPTZ NOT NULL    -- migrated from TIMESTAMP
 ├─ StartPercent      INT NOT NULL
 ├─ ... (other fields unchanged)
 └─ idx_charging_vehicle_starttime ON (VehicleId, StartTime)
```

The new RPCs share a simple aggregation SQL pattern:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION,
  COUNT(*)::INT
FROM charging c
WHERE <date filter on c.StartTime, using session TZ implicitly>
  AND (length($N) = 0 OR c.VehicleId::text = $N);
```

No `AT TIME ZONE` in the WHERE clause — `TIMESTAMPTZ` + `EXTRACT` /
`::date` already use session TZ. The session TZ is whatever PG is
running with (postmaster env `TZ=Asia/Shanghai` or `postgresql.conf`
`timezone = 'Asia/Shanghai'`).

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

The 3 new field-number ranges (ChargingReport 1-7, requests 1-4) do
**not** collide with existing message fields in this file (verified by
reviewer — no proto regeneration errors).

## 6. SQL Design

### 6.1 Schema migration (NEW in v2)

`sql/001_initial.sql` is updated to declare TIMESTAMPTZ columns from
the start (so fresh installs get the correct schema). For existing
databases, a new migration file
`sql/002_charging_timestamptz_migration.sql` performs the ALTER:

```sql
-- sql/002_charging_timestamptz_migration.sql
-- Migrate charging.StartTime / EndTime from TIMESTAMP to TIMESTAMPTZ.
-- Idempotent: only runs the ALTER if the column is still bare TIMESTAMP.
-- Existing TIMESTAMP values are interpreted as UTC; this matches the
-- dev environment's consistent convention and the spec's intent for
-- production deployments (a production deploy must validate this
-- assumption before applying).

BEGIN;

DO $$ BEGIN
  -- Guard checks BOTH columns to be robust against partial-migration
  -- states (e.g. someone ran a half-finished migration manually). If
  -- either column is still bare TIMESTAMP, we run the ALTER — and PG
  -- will be a no-op for any column already TIMESTAMPTZ.
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'charging'
      AND column_name IN ('StartTime', 'EndTime')
      AND data_type = 'timestamp without time zone'
  ) THEN
    ALTER TABLE charging
      ALTER COLUMN StartTime TYPE TIMESTAMPTZ USING StartTime AT TIME ZONE 'UTC',
      ALTER COLUMN EndTime   TYPE TIMESTAMPTZ USING EndTime   AT TIME ZONE 'UTC';
  END IF;
END $$;

COMMIT;
```

Update `scripts/load_schema.sh` to apply both files in order:
`001_initial.sql` then `002_charging_timestamptz_migration.sql`.

Update `tests/fixtures/CMakeLists.txt` to point
`EVGRPC_TEST_SQL_PATH` at a "combined" path that includes both
migrations. Implementation choice: a small wrapper SQL file
`sql/_test_schema.sql` that does `\i 001_initial.sql` then
`\i 002_*.sql` would require psql; simpler is to concatenate at
CMake-configure time. The implementation plan picks the concrete
approach.

### 6.2 SQL per RPC (after TIMESTAMPTZ migration)

**GetDailyChargingReport**:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE c.StartTime::date = make_date($1, $2, $3)
  AND (length($4) = 0 OR c.VehicleId::text = $4);
```

`timestamptz::date` uses session TZ implicitly, so this gives the date
in the operator's local TZ.

**GetMonthlyChargingReport**:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE EXTRACT(YEAR  FROM c.StartTime) = $1
  AND EXTRACT(MONTH FROM c.StartTime) = $2
  AND (length($3) = 0 OR c.VehicleId::text = $3);
```

`EXTRACT` on TIMESTAMPTZ also uses session TZ implicitly.

**GetAnnualChargingReport**:

```sql
SELECT
  COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION       AS total_cost,
  COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh,
  COUNT(*)::INT                                    AS count
FROM charging c
WHERE EXTRACT(YEAR FROM c.StartTime) = $1
  AND (length($2) = 0 OR c.VehicleId::text = $2);
```

### 6.3 Notes

- **`COUNT(*)` is unconditional** — it returns 0 on empty match, not
  NULL. No `COALESCE` needed.
- **`COALESCE(SUM(col), 0)`** is required because `SUM` over zero rows
  returns NULL, not 0. Without this the empty-result row would have
  NULLs in `total_cost`/`total_kwh`.
- **No `AT TIME ZONE` in the WHERE clause.** After migration,
  `TIMESTAMPTZ` + `EXTRACT` / `::date` already uses session TZ. This is
  a deliberate simplification vs. v1 of this spec.
- **No precursor `EXISTS` pre-check.** The existing
  `GetMonthlyReport`/`GetAnnualReport` have an "EXISTS pre-check fires
  INTERNAL when zero rows match" pattern that causes
  `grpc::StatusCode::INTERNAL` to be returned when the requested period
  has no data. We **deliberately do not replicate this** — new RPCs
  return OK with zeros (see §2.5).
- **`length($N) = 0`** for the optional `vehicle_id` filter — same
  pattern as `GetMonthlyReport`. The single inline form is used (no
  dynamic SQL appending); params are always 4 / 3 / 2 regardless of
  whether `vehicle_id` is set. Implementation may choose to inline
  `AND c.VehicleId = $N::text` conditionally for clarity; either is
  fine.

## 7. Validation

| RPC | Field | Rule | Error |
|---|---|---|---|
| `GetDailyChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |
| | `month` | `1..=12` | `INVALID_ARGUMENT` |
| | `day` | `1..=days_in_month(year, month)` | `INVALID_ARGUMENT` |
| `GetMonthlyChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |
| | `month` | `1..=12` | `INVALID_ARGUMENT` |
| `GetAnnualChargingReport` | `year` | `>= 1900` | `INVALID_ARGUMENT` |

**Day-vs-month validity (Feb 30, Apr 31, etc.):** VALIDATED
server-side (v1 said "not validated" — that was wrong because
`make_date(year, 2, 30)` raises SQLSTATE 22008 which becomes INTERNAL
via `ToGrpcStatus`, contradicting the v1 §8 row). The validator
computes the last valid day for the given `(year, month)`:

```cpp
int last_day = 31;
if (month == 2) {
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  last_day = leap ? 29 : 28;
} else if (month == 4 || month == 6 || month == 9 || month == 11) {
  last_day = 30;
}
if (req->day() < 1 || req->day() > last_day) {
  return INVALID_ARGUMENT;
}
```

A small helper (`util::LastDayOfMonth(int year, int month)`) is added
to `src/util/` so the validator stays readable.

## 8. Error Semantics

| Scenario | Response |
|---|---|
| Valid args, data exists | `OK` + `ChargingReport` with real aggregates |
| Valid args, no data for that period | `OK` + `ChargingReport{total_cost=0, total_kwh=0, count=0}` |
| Valid args, far-future date (e.g. year=2099) | `OK` + zero `ChargingReport` |
| Invalid args (year<1900, month out of range, day out of range, Feb 30 etc.) | `INVALID_ARGUMENT` |
| DB transient error | `INTERNAL` (via existing `ToGrpcStatus` mapping) |

This deliberately differs from `GetMonthlyReport`/`GetAnnualReport`
which return `INTERNAL "no aggregate row"` for empty result sets. The
new RPCs establish a cleaner pattern.

## 9. Tests (`tests/integration/display_service_test.cc`)

All tests run in the shared PG (per existing `ServiceITBase` fixture).
After the migration in §6.1, `charging.StartTime` is TIMESTAMPTZ. PG
session TZ defaults to whatever the postmaster was started with —
tests inherit that. CI/dev default: assume `UTC`. A single TZ-boundary
test sets session TZ explicitly via `SET TIME ZONE 'Asia/Shanghai'`
and resets it.

### 9.1 Common assertions

For "happy path with multiple rows":
- Create 3 charging events for one vehicle on the **same local day**
  with distinct `StartTime` values spanning that day (use
  `now()` + offset within the day, since session TZ is UTC in default
  test runs).
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
| `GetDailyChargingReport_Feb30_InvalidArgument` | daily | day=30, month=2 → INVALID_ARGUMENT (was v1's "OK + zero") |
| `GetDailyChargingReport_Apr31_InvalidArgument` | daily | day=31, month=4 → INVALID_ARGUMENT |
| `GetDailyChargingReport_LeapYear_Feb29_HappyPath` | daily | Feb 29 in leap year → OK |
| `ChargingReportTzTest.Daily_AsiaShanghai_RollsToNextDay` (unit) | daily | charging at 23:30Z grouped in Asia/Shanghai next day (see §9.3 for why this is a unit test, not a gRPC IT) |
| `GetMonthlyChargingReport_HappyPath_MultipleRows` | monthly | events spanning 3 days in same month → aggregates |
| `GetMonthlyChargingReport_Empty` | monthly | no data → OK + zeros |
| `GetMonthlyChargingReport_VehicleFilter` | monthly | filter |
| `GetMonthlyChargingReport_YearBelow1900_InvalidArgument` | monthly | year=1899 |
| `GetMonthlyChargingReport_MonthOutOfRange_InvalidArgument` | monthly | month=0 / 13 |
| `GetAnnualChargingReport_HappyPath_MultipleRows` | annual | events across 2 months → aggregates |
| `GetAnnualChargingReport_Empty` | annual | no data → OK + zeros |
| `GetAnnualChargingReport_VehicleFilter` | annual | filter |
| `GetAnnualChargingReport_YearBelow1900_InvalidArgument` | annual | year=1899 |

**~19 new TEST_F** (was 17 in v1; added Apr31 + LeapYear Feb29 for
the day-validity validator coverage).

### 9.3 TZ-boundary test specifics

```cpp
TEST_F(DisplayServiceIT, GetDailyChargingReport_TzBoundary_MidnightRollsToNextDay) {
  auto conn = pg()->acquire();   // acquire a connection for SET TIME ZONE
  pqxx::work tx(*conn);
  tx.exec("SET TIME ZONE 'Asia/Shanghai'");
  tx.commit();

  // Create a charging event at 2026-08-12T20:00:00Z (UTC),
  // which is 2026-08-13T04:00:00+08:00 (Shanghai) — i.e. it
  // should be counted in the 2026-08-13 Shanghai daily report.
  // Note: with TIMESTAMPTZ storage, the instant is preserved
  // unambiguously across INSERT regardless of session TZ.

  ...

  // RESET TIME ZONE so subsequent tests don't see the Shanghai session TZ.
  pqxx::work tx2(*conn);
  tx2.exec("RESET TIME ZONE");
  tx2.commit();
}
```

Because the migration gives us TIMESTAMPTZ, **the test fixture does
NOT need to RESET TIME ZONE in every SetUp** — the TZ-boundary test
cleans up after itself. This removes the fixture-wide concern flagged
by the v1 reviewer.

### 9.4 Migration tests (NEW in v2)

A dedicated test that the schema migration is idempotent and works on
both fresh and existing data:

```cpp
// In tests/unit/test_schema_migration.cc (or as a new IT):
TEST(SchemaMigration, ChargingTimestamptzMigrationIsIdempotent) {
  // 1. Apply 001 + 002 once.
  // 2. Verify column type is TIMESTAMPTZ.
  // 3. Apply 002 again.
  // 4. Verify still TIMESTAMPTZ (no error).
  // 5. Insert a row, read it back, verify the instant is preserved
  //    regardless of session TZ at insert.
}
```

## 10. Known Limitations / Follow-ups

1. **Old RPCs mixed-TZ after migration.** `GetMonthlyReport` /
   `GetAnnualReport` continue to mix charging (now session-TZ via
   TIMESTAMPTZ) with consumption (still UTC via bare TIMESTAMP). For a
   charging event at UTC midnight in a non-UTC TZ, the cost/kwh line
   will be in a different month than the km line. This is an existing
   inconsistency amplified by the migration. Follow-up: separate spec
   to migrate `consumption.Start`/`EndTime` to TIMESTAMPTZ (touches
   `src/services/consumption_service.cc` + `ParseTimestamp`).

2. **No `config.json` TZ field.** PG session TZ is set by postmaster
   `TZ` env or `postgresql.conf timezone =`. Operators running PG via
   `docker run -e TZ=...` get it for free; operators using managed PG
   services (RDS, Cloud SQL) must set the cluster's TZ config. README
   / deployment docs need a one-paragraph update mentioning this
   (out of scope for this spec).

3. **No pagination.** Each report returns aggregates over a single
   period — no row-level pagination. For row-level charging list with
   day filters, the existing `ListChargings` `start_after` /
   `start_before` filters can be used (note: range filter is currently
   untested — a follow-up spec could add coverage).

4. **No aggregation caching.** Reports run `SUM`/`COUNT` from PG on
   every call. Acceptable for now (charging table is small, a single
   user's data).

5. **Redundant `::timestamptz` casts in charging_service.cc remain.**
   After the column migration, casts at lines 210, 328, 408-414 are
   no-ops but harmless. Cleanup is a separate spec.

## 11. Files Touched

```
proto/evgrpc/display.proto                                       [MOD] +ChargingReport, +3 requests, +3 rpc
src/proto/evgrpc/display.pb.cc + display.pb.h                     [AUTO] protoc regen
src/proto/evgrpc/display_grpc.pb.cc + display_grpc.pb.h           [AUTO] protoc regen
src/services/display_service.h                                   [MOD] +3 handler declarations
src/services/display_service.cc                                  [MOD] +3 handler impls (~150 lines)
src/util/last_day_of_month.{h,cc}                                [NEW] small helper for §7 validator
sql/001_initial.sql                                              [MOD] TIMESTAMP → TIMESTAMPTZ for charging
sql/002_charging_timestamptz_migration.sql                       [NEW] idempotent ALTER for existing DBs
sql/_test_schema.sql                                             [NEW] combined schema loader for tests
tests/fixtures/CMakeLists.txt                                    [MOD] EVGRPC_TEST_SQL_PATH → sql/_test_schema.sql
scripts/load_schema.sh                                           [MOD] apply both 001 + 002 in order
tests/integration/display_service_test.cc                        [MOD] +~19 TEST_F
tests/integration/test_schema_migration.cc (or tests/unit/)      [NEW] idempotency + instant-preservation test
docs/superpowers/specs/2026-08-13-display-charging-reports-design.md  [MOD] this v2 doc
```

## 12. Out of Scope, but Mentioned

- README updates documenting the new RPCs and PG session TZ convention
  (covered by this spec's implementation plan, not the spec itself).
- Updating MEMORY.md with "TIMESTAMPTZ schema migration" entry once
  shipped.
- Migrating `consumption.Start`/`EndTime` to TIMESTAMPTZ — separate
  spec.
- Removing now-redundant `::timestamptz` casts in charging_service.cc —
  separate cleanup spec.