# test_consumption.md

## Overview
- **Service:** ConsumptionService
- **RPCs:** CreateConsumption, GetConsumption, UpdateConsumption, DeleteConsumption, ListConsumptions
- **Total tests:** 20 (was 19; +1 added in Phase A for pagination bug fix)
- **FK deps:** vehicle_id (required NOT NULL); weather_id (DB nullable, but production requires non-empty — see "Constraints" below).

## TestHappyPath

### test_create_consumption_returns_id_and_fields
- **RPC:** CreateConsumption
- **Helper:** `vehicle_and_weather` fixture creates both FK refs in one with-block.

### test_get_consumption_returns_created
- **RPC:** GetConsumption
- **Inside `with`** — same cleanup rationale.

### test_update_consumption_changes_end_percent
- **RPC:** UpdateConsumption

### test_delete_consumption_removes_row
- **RPC:** DeleteConsumption
- **`ti.unregister`** after Delete.

### test_list_consumptions_after_create_includes_new
- **RPC:** ListConsumptions
- Filter by vehicle_id; new consumption appears.

## TestErrorPath

### test_get_consumption_unknown_id_returns_not_found
### test_update_consumption_unknown_id_returns_not_found
### test_delete_consumption_unknown_id_returns_not_found
- All: random UUID → NOT_FOUND.

## TestBoundaries

### test_create_consumption_end_equal_start_returns_invalid
- **App validation:** end > start.

### test_create_consumption_end_percent_geq_begin_returns_invalid
- **App validation:** end_percent < begin_percent (consumption drains battery).

### test_create_consumption_highest_lt_lowest_temp_returns_invalid
- **App validation:** highest_temperature_c >= lowest_temperature_c.

### test_create_consumption_highest_eq_lowest_temp_ok
- **App validation boundary**: highest == lowest is OK (`>=` not `>`).

### test_create_consumption_negative_begin_percent_accepted
- **Documents production**: negative percent accepted (no app-level validation).

### test_create_consumption_negative_temperature_accepted
- **Documents production**: negative Celsius temperatures accepted.

### test_create_consumption_early_ev_year_accepted
- **Documents production**: TIMESTAMPTZ allows any year (1990).

## TestConstraints

### test_create_consumption_invalid_vehicle_id_returns_invalid_argument
- **FK violation** (vehicle.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_consumption_invalid_weather_id_returns_invalid_argument
- **FK violation** (weather.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_consumption_empty_weather_id_returns_invalid
- **App validation (Phase 3 fix):** `ValidateConsumption` now rejects empty weather_id with INVALID_ARGUMENT. WeatherId is NOT NULL in the DB schema, so the empty string would otherwise hit NOT NULL and produce INTERNAL via the `not_null_violation` → INTERNAL fallback in error.cc.

### test_update_consumption_empty_weather_id_returns_invalid
- **UpdateConsumption** uses the same `ValidateConsumption` (via a constructed CreateConsumptionRequest-shaped view). Adding this test confirmed the fix works in the update path too.



### test_list_consumptions_invalid_page_token_returns_invalid
- **Phase A fix**: non-numeric page_token → INVALID_ARGUMENT (was INTERNAL).

## Cross-service (Chunk 6 enables Vehicle FK-delete test)

`tests/python/test_vehicle.py::TestConstraints::test_delete_vehicle_with_consumption_returns_invalid_argument` was previously `@pytest.mark.skip`'d (waits for Chunk 6). Chunk 6 implementation removes the skip — that test now runs and verifies:
1. Create vehicle V
2. Create weather W
3. Create consumption C referencing V (and W for weather_id)
4. Try DeleteVehicle(V) → INVALID_ARGUMENT (FK violation, vehicle.Id has consumption rows pointing to it)
5. Vehicle NOT deleted; cleanup handles both rows.

Per `error.cc`: `foreign_key_violation` → INVALID_ARGUMENT (the round-1 review fix).