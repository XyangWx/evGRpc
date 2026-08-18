# test_auth_enforcement.md

## Overview
- **Service:** WeatherService (just a probe target)
- **Total tests:** 3
- **Purpose:** Verify evgrpc's JWT validator rejects invalid tokens.

## TestHappyPath (validation paths)

### test_no_token_returns_unauthenticated
- **Purpose:** No `Authorization` header → UNAUTHENTICATED.
- **Setup:** Bare insecure_channel (no Bearer interceptor).
- **Action:** SearchWeather() with no metadata.
- **Expected:** `grpc.StatusCode.UNAUTHENTICATED`.

### test_malformed_token_returns_unauthenticated
- **Purpose:** Malformed Bearer token (not a real JWT) → UNAUTHENTICATED.
- **Action:** metadata=("authorization", "Bearer not.a.real.jwt").
- **Expected:** UNAUTHENTICATED.

### test_forged_token_returns_unauthenticated
- **Purpose:** Forged JWT signed with throwaway RSA key (real iss/aud, fake kid) → UNAUTHENTICATED.
- **Setup:** Generate RSA-2048 key in-process; sign JWT with claims matching real IdP's iss/aud but throwaway signing key + `kid: "forged-key"`.
- **Action:** Send SearchWeather with `Authorization: Bearer <forged>`.
- **Expected:** UNAUTHENTICATED (server's JWT validator looks up `kid` in JWKS, doesn't find "forged-key", signature fails).

## Claims used (verified against real minted token)

- `iss = "https://auth-test.mksword.com/"`
- `aud = "https://www.mksword.com/grpc/ev"`
- `kid = "19C1B8A78C7648CA5DC4EAEFDEE51986991DBC51"` (real; not used in forged test — we use "forged-key")

## NOT tested (out of scope per spec §10)

- RBAC / per-RPC scope enforcement (waiting on server-side scope checks).
- Expired-token test (requires IdP admin change — can't issue short-lived tokens via the public client_credentials flow).