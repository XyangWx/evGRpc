# test_charging.md

## Overview
- **Service:** ChargingService
- **RPCs:** CreateCharging, GetCharging, UpdateCharging, DeleteCharging, ListChargings
- **Total tests:** 23 (was 22; +1 added in Phase A for pagination bug fix)
- **FK deps:** every Create/Update requires a valid vehicle_id + source_category_id (UUID).

## TestHappyPath

### test_create_charging_returns_id_and_fields
- **RPC:** CreateCharging
- **Purpose:** Create with valid vehicle + source_category + valid fields returns UUID + echoed fields.
- **Helper:** `_make_charging_req(vehicle_id, source_category_id)` builds a valid request; `vehicle_and_source` fixture provides FK refs.
- **Cleanup:** TrackedInsert.

### test_get_charging_returns_created
- **RPC:** GetCharging
- **Purpose:** Round-trip Get inside `with` block (avoid cleanup-deletes-row bug from Weather chunk).

### test_update_charging_changes_kwh
- **RPC:** UpdateCharging
- **Purpose:** Update kwh_charged; verify response reflects change.
- **Inside `with`** — same cleanup rationale.

### test_delete_charging_removes_row
- **RPC:** DeleteCharging
- **Purpose:** Delete removes row; subsequent Get → NOT_FOUND.
- **`ti.unregister(created.id)`** after Delete (round-1 review fix).

### test_list_chargings_after_create_includes_new
- **RPC:** ListChargings
- **Purpose:** New charging appears in List when filtered by vehicle_id.

## TestErrorPath

### test_get_charging_unknown_id_returns_not_found
### test_update_charging_unknown_id_returns_not_found
### test_delete_charging_unknown_id_returns_not_found
- All three: random UUID → NOT_FOUND.

## TestBoundaries

### test_create_charging_end_time_equal_start_returns_invalid
- **App validation** (`charging_service.cc:ValidateCharging`): end_time > start_time.
- Equal → INVALID_ARGUMENT.

### test_create_charging_end_percent_equal_start_returns_invalid
- **App validation:** end_percent > start_percent.

### test_create_charging_kwh_zero_returns_invalid
- **App validation:** kwh_charged > 0.

### test_create_charging_cost_zero_returns_invalid
- **App validation:** cost > 0.

### test_create_charging_location_length
- **VARCHAR(100):** 101-char location → data_exception → INVALID_ARGUMENT.

### test_create_charging_location_at_limit_ok
- **VARCHAR(100):** 100-char location at limit → OK + echoed with len 100.

### test_create_charging_negative_kwh_returns_invalid
- **App validation**: kwh_charged > 0; -1.0 rejected.

### test_create_charging_negative_cost_returns_invalid
- **App validation**: cost > 0; -50.0 rejected.

### test_create_charging_slow_charger_type_ok
- CHARGER_TYPE_SLOW is valid alternative to FAST.

### test_create_charging_with_service_fee_ok
- DoubleValue wrapper with value=5.0; round-trip OK.

### test_create_charging_no_service_fee_is_null
- DoubleValue unset → response.HasField("service_fee") == False.

## TestConstraints

### test_create_charging_invalid_vehicle_id_returns_invalid_argument
- **FK violation** (vehicle.Id doesn't exist) → `foreign_key_violation` → INVALID_ARGUMENT (per `error.cc`).

### test_create_charging_invalid_source_category_id_returns_invalid_argument
- **FK violation** (source_category.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_charging_charger_type_unspecified_returns_invalid
- **App validation (Phase 3 fix):** `ValidateCharging` rejects UNSPECIFIED with INVALID_ARGUMENT. Without this, ChargerTypeLabel('') → NOT NULL violation → INTERNAL (far less helpful).

### test_list_chargings_invalid_page_token_returns_invalid
- **Phase A fix**: non-numeric page_token → INVALID_ARGUMENT (was INTERNAL via `std::stoi` throw caught by generic catch → default INTERNAL).