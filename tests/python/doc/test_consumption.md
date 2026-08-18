# test_consumption.md

## Overview
- **Service:** ConsumptionService
- **RPCs:** CreateConsumption, GetConsumption, UpdateConsumption, DeleteConsumption, ListConsumptions
- **Total tests:** 14
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

## TestConstraints

### test_create_consumption_invalid_vehicle_id_returns_invalid_argument
- **FK violation** (vehicle.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_consumption_invalid_weather_id_returns_invalid_argument
- **FK violation** (weather.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_consumption_empty_weather_id_returns_invalid
- **Production bug discovered:** WeatherId column is nullable in DB (`sql/001_initial.sql:39`), but production code binds `$13` as raw string. Empty string fails UUID cast → INVALID_ARGUMENT.
- The SQL comment claims NULLIF('') wrap, but the actual SQL is `$13` only.
- Test documents current behavior. Fixing production (changing SQL to `$13::text NULLIF ...`) is out of scope; when fixed, this test should be updated to expect OK + response with weather_id="" or similar.

## Cross-service (Chunk 6 enables Vehicle FK-delete test)

`tests/python/test_vehicle.py::TestConstraints::test_delete_vehicle_with_consumption_returns_invalid_argument` was previously `@pytest.mark.skip`'d (waits for Chunk 6). Chunk 6 implementation removes the skip — that test now runs and verifies:
1. Create vehicle V
2. Create weather W
3. Create consumption C referencing V (and W for weather_id)
4. Try DeleteVehicle(V) → INVALID_ARGUMENT (FK violation, vehicle.Id has consumption rows pointing to it)
5. Vehicle NOT deleted; cleanup handles both rows.

Per `error.cc`: `foreign_key_violation` → INVALID_ARGUMENT (the round-1 review fix).