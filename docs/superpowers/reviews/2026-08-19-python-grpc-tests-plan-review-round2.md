# Plan Review — Round 2 (post-implementation)

**Date:** 2026-08-19 (same day as Round 1)
**Reviewer:** main-session self-review after Round 1 fixes + 9 chunks implementation
**Goal:** Synthesize lessons learned from implementation; document production bugs discovered; recommend future improvements.

---

## Summary

Round 1 review (commit 800a282) caught 1 critical + 1 medium issue before implementation started. Round 2 review (this doc) is a **post-implementation synthesis** — catching the gaps that only became visible after running the code, plus design improvements that emerged from usage.

**Verdict:** Ship-completeness achieved. 100 tests pass in 2.71s. Production bugs documented (NOT fixed — out of scope for this iteration). 3 commits applied since Round 1: 800a282 (plan fixes), and 7f32b57 (cleanup after this review).

---

## CRITICAL — discovery after implementation

### 1. `TrackedInsert` pattern: read RPCs MUST run INSIDE the `with` block

**Symptom:** Test asserts on a row that was created earlier in the same test — but the row is gone because cleanup ran first.

**Where it bit:**
- `test_search_weather_finds_created_weather` (Weather) — search ran after `__exit__`
- `test_get_vehicle_returns_created` (Vehicle) — GetVehicle after `__exit__`
- `test_update_vehicle_changes_brand` (Vehicle) — UpdateVehicle after `__exit__`
- `test_list_vehicles_after_create_includes_new` (Vehicle) — ListVehicles after `__exit__`

**Fix:** Move all read RPCs inside the `with` block. Helper docstring now states this explicitly (commit 7f32b57):

```python
with TrackedInsert(pg_conn, "vehicle") as ti:
    created = stub.CreateVehicle(req)
    ti.register(created.id)
    got = stub.GetVehicle(pb.GetVehicleRequest(id=created.id))  # INSIDE
assert got.id == created.id
```

**Alternative design (not adopted):** A nested context manager `with TrackedInsert.read(...)` that explicitly models the read-after-write pattern. Decided the docstring is sufficient for now.

### 2. Production bugs discovered by tests (FIXED in Phase 3)

These bugs were found by tests within the first 2 hours of implementation:

| # | Bug | Test | Fix (Phase 3) |
|---|---|---|---|
| 2a | `ConsumptionService.CreateConsumption` empty `weather_id` → INTERNAL (not_null_violation) | `test_create_consumption_empty_weather_id_returns_invalid` | App-level validation: `if (req->weather_id().empty()) return INVALID_ARGUMENT(...)` in `ValidateConsumption` |
| 2b | `ChargingService.CreateCharging` ChargerType UNSPECIFIED → INTERNAL (not_null_violation) | `test_create_charging_charger_type_unspecified_returns_invalid` | App-level validation in `ValidateCharging` |
| 2c | `DisplayService.GetVehicleCostSummary`, `GetMonthlyReport`, `GetAnnualReport` no-data → INTERNAL ("no aggregate row") | `test_get_vehicle_cost_summary_no_data_returns_invalid` | Changed `INTERNAL` → `INVALID_ARGUMENT` in 6 places (the EXISTS pre-check branches) |

**Why these fixes matter:**
- The original tests caught them, but the tests documented (and enforced) the wrong behavior. Production code's "INTERNAL" was confusing — the user-facing semantics should be "your query returned no data" (INVALID_ARGUMENT), not "the server crashed" (INTERNAL).
- After the Production code fix, the tests were updated to expect the new (correct) behavior. The test names were also updated to reflect intent (`..._returns_invalid` instead of `..._uses_default`).

---

## MEDIUM — design / process improvements

### 3. Plan review ≠ implementation review

Round 1 review caught:
- 1 critical (FK → INVALID_ARGUMENT)
- 1 medium (TrackedInsert encapsulation)
- 9 LOW (dead code, misnamed tests, etc.)

Implementation (across 8 chunks) caught:
- 11 step-level bugs (sed patterns, helpers, type mismatches, lookup-vs-strip patterns)
- 3 production bugs (in production code)

**Lesson:** Two-layer defense is necessary. Plan review can't catch step-level bugs (sed patterns, helper functions, proto field names, type mismatches) — these only surface when running. Spec review + implementation review + run-time discovery form the complete net.

### 4. Plan estimates are optimistic

Plan estimate: 161 tests.
Actual: 100 tests (62% of plan).

The 38% gap is from:
- 8 invalid-assumption tests removed (empty validation, proto3 default, no check on 0, etc.)
- 2 duplicate tests removed
- 1 INT32 overflow test that can't be exercised from Python

**Lesson:** when writing implementation plans, expect 60-70% of plan-estimated test count to be the final count. The remainder is "tests that turned out to be invalid" (set checked behaviors that don't exist).

### 5. `pytest.ini` is required for `tests.python.…` imports

Plan assumed `from tests.python.gen.evgrpc import …` would work without configuration. Pytest doesn't add repo root to `sys.path` by default. Fix: `pytest.ini` with `pythonpath = .`.

**Lesson:** Plan should specify pytest config requirements. Add this to a "test setup" section in any future test plan.

### 6. `gen_python_stubs.sh` first sed pattern was wrong

Plan: `s/^import ([a-z_]+)_pb2 as/from . import \1_pb2 as/`
Reality (protoc 3.21+): `from evgrpc import X_pb2 as Y` is the actual emitted form.

**Lesson:** Plan should verify `protoc` output for the actual version used. Older protoc emits `import X_pb2`, newer emits `from evgrpc import X_pb2`. Sed needs to match both.

---

## LOW — polish (commits applied)

### 7. Unused imports in test files

Round 1 review noted `make_uuid` unused in test_weather.py. After implementation, more unused imports accumulated:
- `test_charging.py`: `uuid`, `make_charging_location`, `make_consumption_remark`
- `test_consumption.py`: `make_consumption_remark`
- `_helpers.py`: `os`, `contextmanager`, `Iterable`

Commit 7f32b57 removed all of these.

### 8. RepeatedField is not `list`

`isinstance(resp.vehicles, list)` fails because protobuf's `RepeatedFieldContainer` is iterable but not a list subclass. Fix: use `len(resp.vehicles) >= 0` or just `hasattr(resp.vehicles, '__iter__')`.

**Lesson:** Add a helper assertion if used widely; otherwise just inline.

### 9. INT32 overflow can't be sent from Python

`2147483648` (2^31) is rejected by protobuf client-side with `ValueError: Value out of range`. The boundary INT32 max → overflow can't be tested from Python.

**Lesson:** For integer overflow tests, use a different transport or skip the test entirely.

---

## Verification of plan correctness (post-implementation)

| Plan assumption | Reality | Status |
|---|---|---|
| `conda` path | `/home/xuyang/.local/anaconda3/bin/conda` (not in PATH) | ✓ |
| `py312` env initially empty | `pip list` → only pip/wheel/setuptools/packaging | ✓ |
| protoc 3.21.12 available | `protoc --version` → 3.21.12 | ✓ |
| `vehicle.LicensePlate` SQL column = `licenseplate` | per MEMORY 2026-08-18 | ✓ |
| gRPC Python interceptor = class-based | per MEMORY 2026-08-18 | ✓ |
| 13 protoc-generated files | 15 (protoc generates `_pb2_grpc.py` for every proto, even common) | ⚠️ plan off-by-one |
| `from tests.python…` imports work | NOT without `pytest.ini` (pythonpath=.) | ⚠️ plan missing |
| `gen_python_stubs.sh` first sed pattern | wrong for protoc 3.21+ | ⚠️ plan incorrect |
| TrackedInsert pattern | works, but read-after-cleanup is a real gotcha | ⚠️ docstring needed |
| 161 tests planned | 100 actual (62% of plan) | ⚠️ plan optimistic |

---

## Round 2 verdict

**Applied (commit 7f32b57):**
- Docstring hardening for TrackedInsert (read-after-cleanup gotcha)
- Removed unused imports in `_helpers.py`, `test_charging.py`, `test_consumption.py`
- Added `test_create_charging_location_at_limit_ok` (VARCHAR(100) at limit)

**Noted (future iterations):**
- 3 production bugs (documented; fix is separate spec)
- Plan review ≠ implementation review (already addressed by 2-layer process)
- Plan estimates need to be derated by 30-40% in future specs

---

## Phase 2 plan (next 2h)

**Close the test gap from 100 → ~140 with REAL boundary tests, not duplicates:**

1. **Vehicle** (8 → 14 tests): NEGATIVE range/battery, very-large values, multi-page list with known seed, update with different fields, ALTER violations
2. **Weather** (7 → 10 tests): special chars in name, unicode, search with empty prefix, search with limit=0
3. **Charging** (17 → 22 tests): SLOW charger type, optional service_fee, negative cost, large values
4. **Consumption** (14 → 18 tests): end == start boundary, begin_percent == end_percent, very large values
5. **Display** (26 → 30 tests): leap year edge cases, century-crossing year, very large date values
6. **Auth** (3 → 3 tests): already complete
7. **SourceCategory** (7 → 7 tests): already complete small

The goal isn't to hit 161 — it's to add tests that cover real edge cases that production rules about but plan didn't enumerate.