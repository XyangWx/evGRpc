# test_weather.md

## Overview
- **Service:** WeatherService
- **RPCs:** CreateWeather, SearchWeather
- **Total tests:** 14 (was 7; +7 added in Phase 2 round-2 review)
- **Purpose:** Validate name create/search semantics + VARCHAR(36) length boundaries.

## TestHappyPath

### test_create_weather_returns_id_and_name
- **RPC:** WeatherService.CreateWeather
- **Purpose:** Verify that a valid Create returns the row's UUID + echoed name.
- **Setup:** namespace fixture provides session prefix; name from `make_weather_name(ns)`.
- **Action:** Call `CreateWeather(name=name)`.
- **Expected:** Response has non-empty `id` (UUID) and `name == request.name`.
- **Cleanup:** TrackedInsert deletes by id on __exit__.
- **Related:** Foundation smoke test (`test_smoke.py`) exercises SearchWeather; this is the Create counterpart.

### test_search_weather_finds_created_weather
- **RPC:** WeatherService.SearchWeather
- **Purpose:** Verify that SearchWeather finds a row by prefix (`^@` "starts with").
- **Setup:** Create a row with `name = marker + "-" + make_weather_name(ns)` where marker is a fresh 6-char hex. Total length = 36 (VARCHAR(36) limit).
- **Action:** Call `SearchWeather(prefix=marker, limit=5)` INSIDE the `with TrackedInsert` block (so the row still exists when the search runs).
- **Expected:** At least one match has `m.name == name`.
- **Rationale:** PG `^@` is "starts with"; the marker MUST be at the start of the name, not the end. The `-` separator avoids ambiguity with other tests' uuid-suffix names.
- **Cleanup:** TrackedInsert deletes the row.
- **Related:** None.

## TestErrorPath

### test_create_weather_duplicate_name_returns_already_exists
- **RPC:** WeatherService.CreateWeather
- **Purpose:** UNIQUE constraint on `weather.Name` produces `ALREADY_EXISTS` (per `error.cc`: `unique_violation` → `ALREADY_EXISTS`).
- **Setup:** Create one row with name X, register for cleanup.
- **Action:** Try to create another row with the same name X inside the same `with` block.
- **Expected:** `grpc.RpcError` with code `ALREADY_EXISTS`.
- **Cleanup:** TrackedInsert deletes the first row; the second failed-Create left no row.
- **Related:** Tests `vehicle.LicensePlate` UNIQUE constraint (Chunk 3).

### test_create_weather_empty_name_is_accepted
- **Documents production**: VARCHAR(36) accepts empty string. No app-level validation.

### test_create_weather_unicode_name_is_accepted
- **Documents production**: VARCHAR is UTF-8. Chinese chars work.

### test_create_weather_special_chars_name_is_accepted
- **Documents production**: Special chars (`!@#`) stored as-is. Parameterized SQL prevents injection.

## TestBoundaries

### test_create_weather_name_length
- **Parametrize IDs:** `[1]`, `[35]`, `[36]`, `[37]`
- **RPC:** WeatherService.CreateWeather
- **Purpose:** Verify VARCHAR(36) boundary behavior:
  - `name_len=1`: 1 char accepted; response echoed with len 1.
  - `name_len=35`: 35 chars accepted (just under limit); len 35.
  - `name_len=36`: 36 chars accepted (at limit); len 36.
  - `name_len=37`: 37 chars rejected → `INVALID_ARGUMENT` ("value too long for type character varying(36)").
- **Setup:** `unique = uuid.uuid4().hex` (32 chars); truncate or pad with `'x'` to `name_len`.
- **Action:** Call CreateWeather(name=name) for each `name_len`.
- **Expected:** IDs 1/35/36 → OK + correct len; ID 37 → RpcError INVALID_ARGUMENT.
- **Cleanup:** TrackedInsert deletes accepted rows; rejected row didn't insert.
- **Related:** Same VARCHAR-length pattern tested for `vehicle.LicensePlate` (Chunk 3, `plate_len` 1/15/16) and `vehicle.Brand` (1/36/37).

### test_search_weather_empty_prefix_returns_all
- **Documents production**: PG `^@ ''` matches every row. Submit search inside `with` so the row still exists.

### test_search_weather_limit_zero_uses_default
- **Documents production**: limit=0 → server uses default 50 (no error).

### test_search_weather_negative_limit_uses_default
- **Documents production**: limit=-1 → server uses default 50.

### test_search_weather_no_matches_returns_empty
- Random 32-char hex prefix → 0 matches.

## Removed (from initial plan, found invalid during implementation)

### test_create_weather_empty_name_returns_invalid_argument
- **Why removed:** production code (`weather_service.cc`) does NOT validate empty string; VARCHAR(36) accepts "". The test assumed app-level validation that doesn't exist. Adding it would force production to validate, which is out of scope for the test suite.

### test_create_weather_duplicate_name_returns_already_exists (TestConstraints duplicate)
- **Why removed:** exact duplicate of `TestErrorPath.test_create_weather_duplicate_name_returns_already_exists`. Kept the ErrorPath one (more natural location for an "expect ALREADY_EXISTS" test).