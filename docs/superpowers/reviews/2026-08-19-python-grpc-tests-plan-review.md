# Plan Review — Chunks 2-9 Round 1

**Date:** 2026-08-19
**Reviewer:** main-session self-review (resuming after spec v4 + plan Chunk 1 approval on 2026-08-18)
**Spec:** `docs/superpowers/specs/2026-08-18-python-grpc-tests-design.md` v4 (approved)
**Plan:** `docs/superpowers/plans/2026-08-18-python-grpc-tests.md` — Chunk 1 approved, Chunks 2-9 first review

---

## CRITICAL — must fix before implementation

### 1. FK violations map to `INVALID_ARGUMENT`, not `FAILED_PRECONDITION`

**Evidence:** `src/db/error.cc:10`:
```cpp
if (dynamic_cast<const pqxx::foreign_key_violation*>(&e)) {
    return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
}
```
This contradicts earlier chunks of the same plan (Chunk 3 line 1043 said `FAILED_PRECONDITION` for the vehicle-FK test).

**Locations to fix:**
- **Plan line 1043** (Chunk 3 `test_delete_vehicle_with_consumption_returns_failed_precondition` docstring): `Expected: FAILED_PRECONDITION` → `Expected: INVALID_ARGUMENT`
- **Plan line 1115** (Chunk 5 Step 1 guidance): `FK violations for VehicleId (orphan) and SourceCategoryId (orphan) → FAILED_PRECONDITION` → `... → INVALID_ARGUMENT`
- **Plan line 1145** (Chunk 6 Step 1 guidance): `FK violations for VehicleId, WeatherId → FAILED_PRECONDITION` → `... → INVALID_ARGUMENT`
- **Spec line 156** (Goal 5 / §5.3 narrative): `non-existent parent, expect FAILED_PRECONDITION` → `expect INVALID_ARGUMENT`

**Root cause:** writer likely confused FK violations with "precondition" semantics (the gRPC code `FAILED_PRECONDITION` exists in the protobuf enum), but the production `error.cc` consistently maps SQL FK violations to `INVALID_ARGUMENT` per the project's policy (see MEMORY entry 2026-08-12 "evGRpc error.cc exception mapping").

**Verification:** grep `src/db/error.cc` shows the policy is `FK → INVALID_ARGUMENT` not `FK → FAILED_PRECONDITION`. Writer should ALWAYS consult `error.cc` before assigning a gRPC status code to a SQL error class.

---

## MEDIUM — design / API hygiene

### 2. `TrackedInsert._ids.remove(...)` breaks encapsulation

**Location:** Plan line 818 (Chunk 3 `test_delete_vehicle_removes_row`):
```python
stub.DeleteVehicle(pb.DeleteVehicleRequest(id=created.id))
ti._ids.remove(created.id)  # already deleted, skip L1 cleanup
```

**Issue:** `_ids` is a private attribute. Plan also documents TrackedInsert at line 318-346 without an `unregister()` method.

**Fix options** (pick one during Chunk 1 implementation):
- **(a)** Add `def unregister(self, id: str) -> None` to TrackedInsert (recommended — symmetric with `register`)
- **(b)** Don't register the id in the first place: `stub.DeleteVehicle(...)` BEFORE `ti.register(created.id)`

**Decision:** add `unregister()` to TrackedInsert (option a). Chunk 1's TrackedInsert definition (lines 318-346) gets the new method. Chunk 3's `ti._ids.remove(...)` becomes `ti.unregister(created.id)`.

---

## LOW — noted but not blocking (implementer should be aware)

### 3. Chunk 2 minor dead code
- `from tests.python._helpers import (... make_uuid,)` — unused in test_weather.py
- Redundant `name = name[:name_len]` slice (line ~663) — `name` already has length `name_len`

### 4. Chunk 3 `test_list_vehicles_empty_returns_empty` is misnamed
- Test name implies a namespace filter that doesn't exist (ListVehicles RPC has no namespace param, per `proto/evgrpc/vehicle.proto`)
- The test currently just calls `stub.ListVehicles(page=1, page_size=1)` and asserts the response is a `ListVehiclesResponse` instance — which is always true
- Fix: rename to `test_list_vehicles_returns_response` OR remove (the type-check is trivial)

### 5. Chunk 3 `test_list_vehicles_after_create_includes_new` walks up to 100 pages
- `for page in range(1, 100)` with `page_size=100` = up to 10,000 rows scanned
- Slow + flaky if DB is large (worst case 100 RPCs per test)
- Fix: page_size=100 + walk up to 5 pages. Or query the test vehicle's id directly via a more targeted RPC (none exists, so walk with limit)

### 6. Chunk 3 boundary tests hedge behavior
- `test_create_vehicle_calibrated_range[0]` comment: "0 = INVALID (or accepted? — verify)"
- `test_create_vehicle_battery_capacity[0.0]` comment: "0.0 = INVALID (or accepted?)"
- Need to verify against production `vehicle_service.cc` whether negative/zero is rejected at app layer (likely yes — typical validator), or accepted and stored (less likely for required fields)
- Fix: implementer should `git grep -n "calibrated_range\|battery_capacity"` to see validator before writing the test, OR pin both to INVALID_ARGUMENT and adjust later

### 7. Chunk 3 `test_create_vehicle_missing_required_field` may not test what it claims
- Proto3 fills in default values for omitted scalar fields (e.g., `purchase_date` defaults to 1970-01-01T00:00:00Z, not "missing")
- The test omitting `purchase_date` will actually send a default Timestamp, which PG will store as `1970-01-01`
- Fix: either (a) use a different field that's nullable in proto (none in CreateVehicleRequest), or (b) rename the test to `test_create_vehicle_default_purchase_date_is_accepted` and assert it's stored as 1970-01-01 (NOT actually testing "missing"), or (c) just remove this test
- Recommendation: REMOVE (not a meaningful boundary case in proto3)

### 8. Chunk 7 DisplayService TZ tests
- 3 v1.1.0 RPCs (Daily/Monthly/AnnualChargingReport) are TZ-aware via `c.StartTime::date = make_date(...)`
- Session TZ comes from PG postmaster `TZ` env or `postgresql.conf timezone` — NOT from libpq client
- Test env is Asia/Shanghai (per MEMORY); the RPCs should still work but date-boundary assertions need to align with session TZ
- Fix: implementer should pin test data to dates that are unambiguous across TZ boundaries (e.g., 2024-06-15 — far from DST/spring-forward), or set PG session TZ to UTC for the test namespace

### 9. Chunks 4-7 are header-only stubs
- `Task 4.1`, `5.1`, `6.1`, `7.1` Step 1 say "pattern from Task 2.1" with extension bullets
- Implementer must produce the actual test code following Chunk 2 / Chunk 3 patterns
- Risk: implementer might miss edge cases without explicit boundary lists
- Mitigation: follow Chunk 3's pattern (per-test code blocks + boundary parametrize) for chunks 5-7 (which have non-trivial boundary tables). Chunk 4 (only 10 tests for 2 RPCs) can stay header-only.

---

## Verification of plan correctness (no bugs found)

| Plan assumption | Verification |
|---|---|
| VARCHAR overflow → INVALID_ARGUMENT | `error.cc` data_exception → INVALID_ARGUMENT ✓ |
| UNIQUE violation → ALREADY_EXISTS | `error.cc` unique_violation → ALREADY_EXISTS ✓ |
| `scripts/run_all_tests.sh` exists | `ls scripts/run_all_tests.sh` → exists ✓ |
| `conda env` py312 currently empty | `pip list` → only pip/wheel/setuptools/packaging ✓ |
| protoc 3.21.12 available system-wide | `protoc --version` → 3.21.12 ✓ |
| `vehicle.LicensePlate` SQL column = `licenseplate` (no underscore) | per MEMORY 2026-08-18 ✓ (caught in Chunk 1 round 1) |
| gRPC Python interceptor = class-based `intercept_unary_unary` | per MEMORY 2026-08-18 ✓ (caught in Chunk 1 round 1) |
| 13 protoc-generated files (not 14) — common.proto has no service | spec says 13 ✓ |

---

## Round 1 verdict

- **CRITICAL fix 1**: APPLY (commit in this PR)
- **MEDIUM fix 2**: APPLY (TrackedInsert.unregister + Chunk 3 update)
- **LOW items 3-9**: NOTE for implementer; not blocking

After CRITICAL + MEDIUM fixes, chunks 2-9 are ready for Chunk 1 implementation.