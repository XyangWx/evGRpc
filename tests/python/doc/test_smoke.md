# test_smoke.md

## Overview
- **Service:** WeatherService (sanity)
- **Total tests:** 1
- **Purpose:** end-to-end sanity check that the test stack works
  (conftest fixtures + auth + nginx:80 → evgrpc:50051 → Postgres).
  NOT a coverage test — a guard against the entire stack being
  broken (docker-compose down, OIDC IdP unreachable, schema migration
  dropped a table).

## TestHappyPath

### test_search_weather_with_valid_bearer_succeeds
- **RPC:** WeatherService.SearchWeather
- **Purpose:** validate the full auth + channel + RPC stack works
  end-to-end with a real Bearer token from auth-test.mksword.com.
- **Setup:** None (relies on session fixtures).
- **Action:** Call `SearchWeather(prefix="", limit=1)` with the
  bearer token injected by the `channel` fixture.
- **Expected:** Response is a `SearchWeatherResponse` (no
  UNAUTHENTICATED, no gRPC error). Empty list is acceptable.
- **Cleanup:** None (probe-only test, no DB writes).
- **Related:** Every other test in `tests/python/` depends on this
  stack being healthy. If this test fails, the rest will too.