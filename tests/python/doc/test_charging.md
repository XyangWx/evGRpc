# test_charging.md

## Overview
- **Service:** ChargingService
- **RPCs:** CreateCharging, GetCharging, UpdateCharging, DeleteCharging, ListChargings
- **Total tests:** 16
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

## TestConstraints

### test_create_charging_invalid_vehicle_id_returns_invalid_argument
- **FK violation** (vehicle.Id doesn't exist) → `foreign_key_violation` → INVALID_ARGUMENT (per `error.cc`).

### test_create_charging_invalid_source_category_id_returns_invalid_argument
- **FK violation** (source_category.Id doesn't exist) → INVALID_ARGUMENT.

### test_create_charging_charger_type_unspecified_uses_default
- **UNSPECIFIED enum (0)** — surprising behavior:
  - `ChargerTypeLabel(UNSPECIFIED)` returns `''`.
  - PG enum cast `$N::charger_type_enum` of `''` is NULL.
  - NULL hits `chargertype NOT NULL` constraint → `not_null_violation`.
  - `not_null_violation` is NOT a subclass of `data_exception` in libpqxx.
  - `error.cc` falls through to default → INTERNAL.
- Documents current production behavior. Should ideally be `INVALID_ARGUMENT` (production should validate enum) but changing error.cc is out of scope.