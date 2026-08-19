# test_vehicle.md

## Overview
- **Service:** VehicleService
- **RPCs:** CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles
- **Total tests:** 40 (was 37; +3 added in Phase D for page_size INT_MAX fix)
- **Purpose:** CRUD + boundaries + UNIQUE constraint.

## TestHappyPath

### test_create_vehicle_returns_id_and_brand
- **RPC:** VehicleService.CreateVehicle
- **Purpose:** Verify Create returns UUID + echoed fields.
- **Action:** CreateVehicle with valid fields.
- **Expected:** Non-empty `id`, `brand == req.brand`.
- **Cleanup:** TrackedInsert.

### test_get_vehicle_returns_created
- **RPC:** VehicleService.GetVehicle
- **Purpose:** Round-trip Get returns the same row that Create inserted.
- **Action:** CreateVehicle + GetVehicle (BOTH inside `with TrackedInsert` — GetVehicle after __exit__ would 404 since cleanup already deleted).
- **Expected:** `got.id == created.id`, `got.license_plate == req.license_plate`.
- **Related:** Identical "search-after-cleanup" bug caught in Weather chunk; documented for implementer.

### test_update_vehicle_changes_brand
- **RPC:** VehicleService.UpdateVehicle
- **Purpose:** UpdateVehicle persists changes.
- **Action:** Create + Update (both inside `with`).
- **Expected:** `updated.brand == "test-brand-updated"`.
- **Cleanup:** TrackedInsert.

### test_delete_vehicle_removes_row
- **RPC:** VehicleService.DeleteVehicle
- **Purpose:** DeleteVehicle removes the row; subsequent Get → NOT_FOUND.
- **Action:** Create + DeleteVehicle + GetVehicle(404) — all inside `with`.
- **Note:** After DeleteVehicle, call `ti.unregister(created.id)` (per round-1 review fix) to skip L1 cleanup.
- **Expected:** GetVehicle raises `NOT_FOUND`.

### test_list_vehicles_returns_response
- **RPC:** VehicleService.ListVehicles
- **Purpose:** ListVehicles accepts `page_size` + `page_token` (not `page` — plan's draft had wrong field name).
- **Action:** ListVehicles(page_size=1).
- **Expected:** Returns `ListVehiclesResponse` with iterable `vehicles` (may be empty).
- **Related:** Pagination uses page_token (offset as string), not page numbers.

### test_list_vehicles_after_create_includes_new
- **RPC:** VehicleService.ListVehicles
- **Purpose:** Newly-created vehicle is reachable via paged list.
- **Action:** Create + walk pages (max 10, page_size=100, page_token=offset).
- **Expected:** Found in some page. Cleanup INSIDE `with` (same GetVehicle bug).

## TestErrorPath

### test_get_vehicle_unknown_id_returns_not_found
- **RPC:** VehicleService.GetVehicle
- **Purpose:** Random UUID → NOT_FOUND.

### test_update_vehicle_unknown_id_returns_not_found
- **RPC:** VehicleService.UpdateVehicle
- **Purpose:** Random UUID + valid UpdateVehicleRequest → NOT_FOUND.

### test_delete_vehicle_unknown_id_returns_not_found
- **RPC:** VehicleService.DeleteVehicle
- **Purpose:** Random UUID → NOT_FOUND.

## TestBoundaries

### test_create_vehicle_license_plate_length
- **Parametrize IDs:** `[1-True]`, `[15-True]`, `[16-False]`
- **VARCHAR(15):** 1 = OK, 15 = at-limit OK, 16 = INVALID_ARGUMENT ("value too long").
- **Helper:** `_make_create_req(ns, plate_len)` truncates or pads with 'x'.

### test_create_vehicle_brand_length
- **Parametrize IDs:** `[1-True]`, `[36-True]`, `[37-False]`
- **VARCHAR(36):** 1 = OK, 36 = at-limit OK, 37 = INVALID_ARGUMENT.

### test_create_vehicle_calibrated_range
- **Parametrize IDs:** `[0-True]`, `[1-True]`, `[2147483647-True]`
- **INT column:** 0 = OK (no check), positive = OK, INT32 max (2^31-1) = OK.
- **NOT tested:** INT32 overflow (2^31). Protobuf's int32 wire format rejects it client-side (`ValueError: Value out of range: 2147483648`), so we can't even send it. PG's INT column accepts any int4 value; the boundary only exists in the protobuf layer.

### test_create_vehicle_battery_capacity
- **Parametrize IDs:** `[0.0-True]`, `[0.01-True]`, `[99999999.99-True]`, `[100000000.0-False]`
- **DECIMAL(10,2):** 0 = OK (no check), small = OK, max (99999999.99) = OK, overflow = INVALID_ARGUMENT.

### test_create_vehicle_negative_battery_accepted
- **Documents production**: negative battery accepted (no app-level validation).

### test_create_vehicle_negative_range_accepted
- **Documents production**: negative range accepted.

### test_create_vehicle_empty_brand_accepted
- **Documents production**: empty brand accepted.

### test_create_vehicle_unicode_brand_accepted
- **Documents production**: Chinese brand name accepted (UTF-8).

### test_create_vehicle_earliest_purchase_date_accepted
- **Documents production**: DATE 1900-01-01 accepted (no lower bound).

### test_create_vehicle_future_purchase_date_accepted
- **Documents production**: DATE in future (2099) accepted (no upper bound).

### test_update_vehicle_change_license_plate
- **RPC:** UpdateVehicle with new license_plate (assumes unique).

### test_update_vehicle_change_battery_capacity
- **RPC:** UpdateVehicle with new battery_capacity_kwh.

### test_list_vehicles_paging_returns_consistent
- **RPC:** Create 3 vehicles, walk pages of 1, verify all 3 found.
- **Inside `with` block** — read RPC pattern.

## TestConstraints

### test_create_vehicle_duplicate_license_plate_returns_already_exists
- **UNIQUE constraint** on `vehicle.LicensePlate`.
- Per `error.cc`: `unique_violation` → `ALREADY_EXISTS`.

### test_update_vehicle_to_duplicate_license_plate_returns_already_exists
- Update V2 with V1's plate → UNIQUE hit.
- Same `ALREADY_EXISTS`.

### test_delete_vehicle_with_consumption_returns_invalid_argument
- **Status:** SKIPPED (will be enabled in Chunk 6 after ConsumptionService tests land).
- **FK constraint:** Consumption.VehicleId REFERENCES vehicle(Id). PG default NO ACTION.
- Per `error.cc`: `foreign_key_violation` → `INVALID_ARGUMENT` (not FAILED_PRECONDITION — fixed in round-1 review).
- **Action plan:** Chunk 6 Step 3 removes the `@pytest.mark.skip` decorator.



### test_list_vehicles_invalid_page_token_returns_invalid
- **Phase A fix**: non-numeric page_token → INVALID_ARGUMENT (was INTERNAL).
- `std::stoi` throws `std::invalid_argument` on non-numeric input, falls through `error.cc` to default INTERNAL. Now uses `ParsePageToken` helper.

### test_list_vehicles_overflow_page_token_returns_invalid
- page_token > INT_MAX → INVALID_ARGUMENT (was INTERNAL via `std::out_of_range`).

### test_list_vehicles_empty_page_token_works
- Empty token = first page (offset 0). Documented behavior.



### test_list_vehicles_int_max_page_size_caps_to_1000
- **Phase D fix**: page_size=INT_MAX (2147483647) → OK, capped to 999 (not INT_MAX overflow).
- Bug: page_size + 1 overflowed to INT_MIN, PG returned 'LIMIT must not be negative'.
- Fix: cap page_size at kMaxPageSize-1=999 before +1.

### test_list_vehicles_negative_page_size_uses_default
- page_size <= 0 → server uses default 50. Documents existing behavior.

## Removed (from initial plan, found invalid during implementation)

### test_create_vehicle_empty_brand_returns_invalid_argument
- **Why removed:** no app-level validation; VARCHAR(36) accepts "".

### test_create_vehicle_negative_battery_returns_invalid_argument
- **Why removed:** no app-level validation; DECIMAL(10,2) accepts negative.

### test_create_vehicle_missing_required_field_returns_invalid_argument
- **Why removed:** proto3 fills in defaults for omitted scalar fields. Omitting `purchase_date` sends 1970-01-01T00:00:00Z, not "missing". Test can't distinguish "missing" from "default" in proto3.

### test_create_vehicle_calibrated_range[0-False]
- **Why changed:** Production has no check on 0 (no CHECK constraint, no app validator). 0 is accepted.

### test_create_vehicle_battery_capacity[0.0-False]
- **Why changed:** Same — 0.0 is accepted.