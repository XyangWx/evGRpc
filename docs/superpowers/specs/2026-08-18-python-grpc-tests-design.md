# Python gRPC Integration Test Suite for Deployed evGRpc

- **Date:** 2026-08-18
- **Status:** Design (v3, awaiting plan/implementation)
- **Replaces:** none
- **Supersedes:** v2 of this spec

## 0. Revision History

- **v1 → v2:** Spec reviewer found 4 blocking issues + 6 non-blocking + 8
  recommendations. Resolved:
    - RPC count reconciled: 30 RPCs total (was misstated as 28 in v1).
    - VARCHAR generators rewritten: `namespace` is now bare 8-hex;
      `make_license_plate(ns` / `make_weather_name(ns` etc. add the
      `test-` prefix and a short suffix so every emitted value fits
      the relevant `VARCHAR(n`)` limit.
    - Cleanup ordering: §4 and §8.3 agree on **children-first** order
      (`consumption → charging → vehicle → weather → source_category`).
      Schema FKs verified as bare `REFERENCES` with no `ON DELETE`
      clause → PG default NO ACTION (= RESTRICT), so children MUST be
      removed before parents.
    - Test-count consistency: 161 (not 150/156/161). Budgets recalculated:
      target ≤120s, hard-fail > 120s. Old `<90s / hard-fail > 120s` was
      self-inconsistent.
    - Schema columns verified against `sql/001_initial.sql`. No more
      "`Location` (or `Remark`)" hedging — `charging.Location` is
      VARCHAR(100) NOT NULL by test discipline; `consumption.Remark`
      is TEXT nullable, populated by test discipline.
    - Hard-crash isolation: added **session-start sweep** (`DELETE ...
      WHERE <col> LIKE 'test-%'`) before the per-test run, in addition
      to the existing session-end sweep. Orphans from SIGKILL/OOM are
      cleaned on the next pytest invocation regardless of namespace.
    - "C3" leftover terminology removed. Cleanup layers now named L1
      (per-function) and L2 (per-session).
    - Auth failure paths expanded: replaced "expired-token" test
      (unimplementable without admin UI changes) with "forged token"
      (valid JWT structure, signed with attacker's RSA key — IdP's
      JWKS verification rejects).
    - `auth_token` subprocess failure → explicit `pytest.skip` (matches
      the `channel` fixture's behavior).
    - Token cache race: documented `EVGRPC_CACHE` env var for
      per-invocation isolation in CI; default keeps the shared
      `/tmp/evgrpc_token.json`.
    - Recommendations addressed: `psycopg[binary]` moved under
      `pip:`; TRUNCATE rollback wording fixed; `FutureTimeoutError`
      named correctly; proto layout reconciled (subdir output, not
      flat); Goal 9 vs §5.4 reconciled (stubs committed, gen is
      opt-in); `make_uuid()` purpose documented; `sed -i` GNU note
      added.

- **v2 → v3:** Reviewer found 1 v1 finding still broken (§4 cleanup
  ordering list was parents-first despite annotation), 4 new issues
  in §5.4 gen script, (1 dangling cross-ref in §2.7), and 4 advisory
  recommendations. Resolved:
    - §4 PRE-yield DELETE list reordered to children-first
      (`consumption → charging → vehicle → weather → source_category`),
      matching §8.3. POST-yield is now explicitly children-first too
      (no longer "same as pre-yield", since the old §4 order was
      wrong and would have been inherited).
    - §5.4 sed generalized: pattern now `s/^import ([a-z_]+)_pb2 as/`
      instead of hardcoded `vehicle. Comment also fixed (no longer
      refers to the non-existent `evgrpc_pb2`).
    - §5.4 added second sed for cross-file imports: protoc emits
      `from evgrpc import common_pb2 as ...` in `vehicle_pb2.py` and
      `display_pb2.py`; rewrites to `from . import common_pb2 as ...`.
    - §5.4 layout: removed `common_pb2_grpc.py` from the file list
      (common.proto has no `service` declaration, so protoc does not
      emit a grpc-python counterpart).
    - §2.7 removed dangling "(matching the threshold in §9.1)" —
      §9.1 is a command list with no threshold.
    - §5.2 collision math reworded: 2-hex suffix is for *tie-breaking*
      within a single test, not collision avoidance across the suite.
      Real safety is L1 per-function cleanup (`TrackedInsert` deletes
      the row before the next test runs).
    - §5.1 added `pg_conn` fixture spec: `psycopg` connection string
      comes from `DATABASE_URL` env var, falls back to the docker-compose
      default `postgresql://vegrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc`
      (password URL-encoded because of the embedded `@`).
    - §2.7 added caveat: DisplayService's 43 tests hit multi-table
      aggregates and may exceed the 0.65s/case average; total budget
      target stays ≤120s but per-case budget is approximate, not hard.
    - §5.1 typo: `EVGRPC_CACHE=/tmp/evgrpc_token-<pid>.>.json`
      → `EVGRPC_CACHE=/tmp/evgrpc_token-<pid>.json`.

- **v3 → v4:** User added a new requirement during the writing-plans
  hand-off: every test case must have Markdown documentation for
  future review and augmentation, placed under `tests/python/doc/`.
  Resolved:
    - **Goal 11** added: "All test cases documented in Markdown".
      Mandates a per-test doc section before the test is merged
      (code-review gate).
    - **§5.6** new section: `tests/python/doc/` layout, per-test MD
      template, maintenance convention, and explicit out-of-scope
      decision (no auto-generation; human rationale is the point).
    - **§7** file layout extended to include `tests/python/doc/`
      with the 7 expected MD files (one per test file).
    - **§10** notes that auto-generating these docs is explicitly
      out of scope (loses the rationale cross-references that make
      the docs useful for review/augmentation).

## 1. Background

`evGRpc` ships 6 gRPC services with **30 RPCs** total (verified against
`proto/evgrpc/*.proto` via `grep -c '^  rpc '`):
WeatherService (2) + VehicleService (5) + SourceCategoryService (2)
+ ChargingService (5) + ConsumptionService (5) + DisplayService (11) = 30.

The C++ test suite (`tests/unit/`, `tests/integration/`,
`tests/integration/smoke_e2e_test.cc`) covers the service layer
end-to-end against a real Postgres + an in-process gRPC server. It is
fast, thorough on the C++ side, and runs under `ctest`. What's missing
is a **client-side integration test suite written in Python**:

1. The C++ suite boots its own `TestServer` (in-process gRPC + JWKS
   HTTP + bypass auth) inside the test binary. It does **not**
   exercise the `nginx → evgrpc → Postgres` stack that production
   traffic flows through. Bugs that only show up at the deployment
   boundary — wrong gRPC framing through nginx, HTTP/2 negotiation,
   OIDC bearer-token rejection, response size limits — are invisible
   to the C++ suite.
2. The OIDC interceptor (`src/auth/authenticate.cc`) is exercised in
   C++ by `tests/unit/test_authenticate.cc` (validator only) and the
   C++ integration suite (`authenticate` flag toggle on `TestServer`).
   Neither path runs the *real* OpenIddict at
   `auth-test.mksword.com` end-to-end.
3. Cross-service behavior (e.g. a `Vehicle` row whose `consumption`
   rows are deleted out from under it; or a `Charging` row whose
   `SourceCategory` was renamed to break a display aggregate) is
   tested today only implicitly via ad-hoc smoke runs in
   `scripts/smoke.sh`. There is no Python harness to reproduce.

A Python client-side suite hits the real `nginx:80 → evgrpc:50051 →
Postgres` stack, validates bearer-token enforcement end-to-end with
the actual OIDC IdP, and gives a clean Python entry point for future
behavior-driven test additions.

## 2. Goals

1. **All 30 RPCs have at least one happy-path test** exercised
   through `localhost:80` with a real Bearer token from
   `https://auth-test.mksword.com/` (via the existing `evgrpc-token`
   helper).
2. **All 30 RPCs have at least one error-path test** (NOT_FOUND,
   INVALID_ARGUMENT, ALREADY_EXISTS, FAILED_PRECONDITION,
   UNAUTHENTICATED — whichever is reachable for that RPC).
3. **Data-boundary tests** for the fields with explicit limits in
   `sql/001_initial.sql`: VARCHAR lengths, INT ranges, DECIMAL scale,
   required-vs-optional, empty-string, valid-vs-invalid enum values.
4. **UNIQUE-constraint tests** for `vehicle.LicensePlate`,
   `weather.Name`, `source_category.Name` — insert duplicate, expect
   `ALREADY_EXISTS`.
5. **FK-constraint tests** for `consumption.VehicleId` →
   `vehicle.Id`, `consumption.WeatherId` → `weather.Id`,
   `charging.VehicleId` → `vehicle.Id`,
   `charging.SourceCategoryId` → `source_category.Id` — insert with
   non-existent parent, expect `FAILED_PRECONDITION`.
6. **Auth enforcement tests** at the gRPC layer:
   - no token → `UNAUTHENTICATED`
   - malformed token → `UNAUTHENTICATED`
   - forged token (valid JWT shape, signed with attacker's RSA key)
     → `UNAUTHENTICATED`

   (Scope / RBAC tests deferred — see §3.)
7. **Total runtime ≤ 120s** on the dev VM for the full suite
   (161 cases × ~0.65s avg = ~105s typical). Hard-fail threshold
   is `> 120s`. Per-case budget is approximate — DisplayService's
   43 tests hit multi-table aggregates and may push individual
   cases past 0.65s; aggregate total is what matters. If a future
   change pushes the suite past 120s reliably, the threshold and/or
   per-case budget must be revisited.
8. **No pollution of dev data**: tests insert rows only with a unique
   `test-<random>-` prefix and clean up at function teardown, session
   end, AND session start (defense against hard crashes).
9. **One-command setup**: `conda env create -f environment.yml &&
   conda activate evgrpc-tests && pytest tests/python/ -v` works on a
   fresh clone without re-running the protoc step (stubs are
   committed).
10. The suite **slots into `run_all_tests.sh`** as a final stage
    that fails the script non-zero on any pytest failure.

11. **All test cases documented in Markdown**: every test in
    `tests/python/` has a corresponding section in
    `tests/python/doc/<service>.md` covering setup, action,
    expected behavior, cleanup, and rationale. New tests must add
    their doc section before the test is merged (code-review gate;
    enforced manually in v1, see §5.6). This is the user's
    primary vehicle for **future review and augmentation** —
    rationale for boundary values, cross-references to schema
    constraints, and notes on what other tests cover similar
    surface all live here rather than in inline `# comments`.

## 3. Non-Goals

- RBAC / per-RPC scope enforcement (`evgrpc` does not currently
  enforce per-RPC scopes; the OIDC validator only checks iss/aud/exp).
  When the server grows scope checking, this suite should be
  extended — but not in this spec.
- Expired-token test: requires an IdP admin change (separate client
  with short token lifetime) to mint a token we can then let expire
  cheaply. Out of scope; the 3 auth tests in Goal 6 already cover
  the rejection surface without it.
- Branch-coverage reporting. Pass/fail is the success metric.
- Performance benchmarks, fuzz tests, property-based tests.
- Mock-gRPC-server unit tests. The whole point is to hit the real
  deployed service.
- Re-enabling testcontainers-cpp / ephemeral Postgres. The suite
  uses the existing `evgrpc` database via the existing
  docker-compose stack.
- Migration of the existing C++ integration suite. That suite stays
  as-is and continues to run via `ctest`. The new Python suite is
  additive.
- Auto-start of `docker compose up`. The Python suite expects
  `localhost:80` to already be serving. If it isn't, the session
  is skipped with a clear message (see §6.2).

## 4. Architecture

```
pytest tests/python/ -v
  └─ Session scope (one process):
       ├─ conftest.py::auth_token       # subprocess evgrpc-token, cached
       ├─ conftest.py::channel          # insecure localhost:80, bearer interceptor
       │     - fails fast if server unreachable → pytest.skip
       ├─ conftest.py::namespace        # 8-hex session-unique prefix (no 'test-')
       └─ conftest.py::cleanup_namespace (autouse, session scope)
             PRE-yield (session start):
               DELETE FROM consumption     WHERE remark         LIKE 'test-%'
               DELETE FROM charging        WHERE location      LIKE 'test-%'
                                            OR remark           LIKE 'test-%'
               DELETE FROM vehicle         WHERE license_plate LIKE 'test-%'
               DELETE FROM weather         WHERE name          LIKE 'test-%'
               DELETE FROM source_category WHERE name          LIKE 'test-%'
             (children-first; FKs verified as NO ACTION in §8.3)
             yield
             POST-yield (session teardown):
               DELETE FROM consumption     WHERE remark         LIKE 'test-%'
               DELETE FROM charging        WHERE location      LIKE 'test-%'
                                            OR remark           LIKE 'test-%'
               DELETE FROM vehicle         WHERE license_plate LIKE 'test-%'
               DELETE FROM weather         WHERE name          LIKE 'test-%'
               DELETE FROM source_category WHERE name          LIKE 'test-%'
  └─ Function scope (per test):
       └─ _helpers.py::TrackedInsert context manager
             yield
             on __exit__: DELETE FROM <table> WHERE id = ANY(<ids>)
  └─ Test file per service:
       test_weather.py
       test_vehicle.py
       test_source_category.py
       test_charging.py
       test_consumption.py
       test_display.py
       test_auth_enforcement.py
       └─ TestHappyPath
       └─ TestErrorPath
       └─ TestBoundaries
       └─ TestConstraints
```

The suite is **one process, one channel, one namespace per
`pytest` invocation**. Tests run serially (`pytest-xdist` is not
required). Each test inserts data with `test-<random>-` prefix and
tears down its own rows on function exit. The session-scope
`cleanup_namespace` fixture sweeps `test-%` rows twice: **before** the
session (cleans orphans from any prior SIGKILL/OOM) and **after** the
session (cleans this run). This double-sweep covers all three crash
modes:

- clean exit → both sweeps run
- `pytest` exception → session-end sweep runs (context manager)
- SIGKILL/OOM/power-loss → next pytest run's session-start sweep
  catches the orphans

## 5. Components

### 5.1 `tests/python/conftest.py`

Defines four session-scoped fixtures:

- **`auth_token`** — runs `subprocess.check_output(["evgrpc-token"],
  timeout=15)`. Reused across all tests. The shared cache at
  `/tmp/evgrpc_token.json` means the subprocess is a ~50ms cache hit
  for the entire session. When `evgrpc-token` exits non-zero or
  times out, the fixture raises `pytest.skip("evgrpc-token failed:
  <reason>, skipping Python gRPC IT")` — matches the `channel`
  fixture's skip semantics.

  For parallel/isolated runs (e.g. two pytest invocations in
  different worktrees against the same dev box), set
  `EVGRPC_CACHE=/tmp/evgrpc_token-<pid>.json` to give each run its
  own cache file. (Default keeps the shared cache.)

- **`pg_conn`** — `psycopg` connection to the `evgrpc` database,
  used by the L2a/L2b cleanup sweeps (`cleanup_namespace` fixture)
  and by `TrackedInsert`'s L1 DELETE. Connection string comes from
  the `DATABASE_URL` env var (matching the C++ server's
  `config.json` convention). Falls back to the docker-compose default:
  `postgresql://vegrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc`
  (password URL-encoded because of the embedded `@`; see the
  `evGRpc 测试约定` entry in MEMORY.md). Tests that don't need DB
  access (e.g. `test_auth_enforcement.py`) simply don't reference
  `pg_conn`, so pytest doesn't instantiate it for them.

- **`channel`** — creates an insecure gRPC channel to `localhost:80`,
  installs a metadata-injecting interceptor that adds
  `authorization: Bearer <auth_token>` to every call. Validates the
  channel with `grpc.channel_ready_future(chan).result(timeout=5)`.
  If `grpc.FutureTimeoutError` is raised, the fixture calls
  ``pytest.skip("evgrpc:80 unreachable, skipping Python gRPC IT")``.
  The `TestAuthEnforcement` file uses its own bare channel (no
  interceptor) for the missing-header / bad-token tests — see §5.5.

- **`namespace`** — generates `uuid.uuid4().hex[:8]` once per
  session (8-char random hex, **no `test-` prefix**). The helpers in
  §5.2 add the `test-` prefix and a short suffix so generated
  identifiers fit VARCHAR limits.

- **`cleanup_namespace`** — `autouse=True`, `scope="session"`. Runs
  the children-first DELETE sweep both **before** and **after** the
  yield. Logs per-table row counts deleted in the post-yield sweep.
  Wraps the post-yield DELETE in `try/except Exception as e: log
  warning, continue` — cleanup failures must not mask test results.

### 5.2 `tests/python/_helpers.py`

- **`class TrackedInsert`** — context manager yielding a closure to
  register rows the test inserted (`register(table, id)`). On
  `__exit__` runs `DELETE FROM <table> WHERE id = ANY(<ids>)`. This
  is the per-function cleanup layer (L1).

- **`make_license_plate(ns: str) -> str`** — returns
  `f"test-{ns}{uuid.uuid4().hex[:2]}"`. With `ns` = 8-hex, output
  length = 5 + 8 + 2 = **15 chars exactly** (VARCHAR(15) limit).
  The 2-hex random suffix (256 possible values) is for tie-breaking
  *within a single test*, not cross-test collision avoidance. The
  real safety mechanism is L1 per-function cleanup: every test that
  creates a `vehicle` row registers the row in `TrackedInsert`, and
  the context manager's `__exit__` `DELETE`s it before the next
  test starts. The session-unique `namespace` prefix is what keeps
  parallel pytest invocations from colliding.

- **`make_weather_name(ns: str) -> str`** — returns
  `f"test-{ns}{uuid.uuid4().hex[:16]}"`. Length = 5 + 8 + 16 = 29
  chars (VARCHAR(36) limit, 7-char margin). Same form for
  `make_source_category_name(ns)`.

- **`make_charging_location(ns: str) -> str`** — returns
  `f"test-loc-{ns}{uuid.uuid4().hex[:8]}"`. Length = 9 + 8 + 8 = 25
  chars (VARCHAR(100) limit, comfortable headroom). Used to populate
  `charging.Location` so cleanup `LIKE 'test-%'` matches.

- **`make_consumption_remark(ns: str) -> str`** — returns
  `f"test-rem-{ns}{uuid.uuid4().hex[:8]}"`. Length = 9 + 8 + 8 = 25
  chars; `Remark` is `TEXT` (no limit), but the `test-` prefix is
  what makes cleanup `LIKE 'test-%'` match.

- **`make_uuid() -> str`** — returns `str(uuid.uuid4())`. Used by
  error-path tests probing `GetXxx` / `UpdateXxx` / `DeleteXxx`
  with a random UUID expected to NOT exist (server generates UUIDs
  for primary keys, so tests don't need to manufacture one).

### 5.3 Per-service test files

Each follows the same shape:

```python
# test_vehicle.py
import grpc, pytest, uuid
from tests.python.gen.evgrpc import vehicle_pb2 as pb
from tests.python.gen.evgrpc import vehicle_pb2_grpc as rpc
from tests.python._helpers import TrackedInsert, make_license_plate, make_uuid

class TestHappyPath:
    def test_create_vehicle_returns_id_and_brand(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        req = pb.CreateVehicleRequest(
            brand=f"test-brand-{namespace}",
            calibrated_range_km=400,
            battery_capacity_kwh=75.0,
            purchase_date=...,
            license_plate=make_license_plate(namespace),
        )
        with TrackedInsert("vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register("vehicle", resp.id)
        assert resp.brand == req.brand
        ...

class TestErrorPath:
    def test_get_vehicle_unknown_id_returns_not_found(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicle(pb.GetVehicleRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

class TestBoundaries:
    @pytest.mark.parametrize("plate_len,expect_ok", [(1, True), (15, True), (16, False)])
    def test_create_vehicle_license_plate_length(self, channel, namespace, plate_len, expect_ok):
        ...

class TestConstraints:
    def test_create_vehicle_duplicate_license_plate_returns_already_exists(self, ...):
        ...
```

Per-service test count breakdown (sum = **161**):

| Service | RPCs | Happy | Error | Boundaries | Constraints | Total |
|---|---:|---:|---:|---:|---:|---:|
| WeatherService | 2 | 2 | 2 | 6 | 2 | 12 |
| VehicleService | 5 | 6 | 5 | 12 | 4 | 27 |
| SourceCategoryService | 2 | 2 | 2 | 4 | 2 | 10 |
| ChargingService | 5 | 6 | 5 | 16 | 8 | 35 |
| ConsumptionService | 5 | 6 | 5 | 14 | 6 | 31 |
| DisplayService | 11 | 12 | 11 | 14 | 6 | 43 |
| Auth enforcement (own file) | — | — | 3 | — | — | 3 |
| **Total** | **30** | **34** | **33** | **66** | **28** | **161** |

**Why "Happy" > RPCs in some rows**: Vehicle/Charging/Consumption
each have one `List*` RPC where the happy test covers both the
"empty list" and "list with one rows" variants (`ListXxx` with no
data → empty response; `ListXxx` after a `CreateXxx` → one row).
Display has one report-style RPC that warrants the same empty/populated
split (Display uses `Get*` verbs, not `List*`, but the same
empty/populated distinction applies). WeatherService / SourceCategory
use `Search*` (not `List*`), which is exercised once because the
search-success path covers both states naturally.

### 5.4 Stub generation: `scripts/gen_python_stubs.sh`

```bash
#!/usr/bin/env bash
# Regenerate Python gRPC stubs from proto/evgrpc/*.proto.
# Re-run only when a .proto file changes (stubs are committed).
# GNU sed -i (no backup arg) is fine on the dev VM (Linux).
set -euo pipefail
cd "$(dirname "$0")/.."
python -m grpc_tools.protoc \
    -I proto \
    --python_out=tests/python/gen \
    --grpc_python_out=tests/python/gen \
    proto/evgrpc/*.proto
# Generated *_pb2_grpc.py uses `import <svc>_pb2 as ...` for each of the
# 6 services (vehicle, weather, source_category, charging, consumption,
# display). Rewrite to package-relative form so the `tests.python.gen.evgrpc`
# package imports resolve correctly.
sed -i -E 's/^import ([a-z_]+)_pb2 as/from . import \1_pb2 as/' \
    tests/python/gen/evgrpc/*_pb2_grpc.py
# Generated *_pb2.py cross-imports sibling messages (e.g. vehicle.proto
# imports evgrpc/common.proto; display.proto imports both common and
# charging). protoc emits `from evgrpc import common_pb2 as ...`; rewrite
# to package-relative form.
sed -i -E 's/^from evgrpc import ([a-z_]+)_pb2/from . import \1_pb2/' \
    tests/python/gen/evgrpc/*_pb2.py
# Generate __init__.py for the gen package and the evgrpc subpackage.
touch tests/python/gen/__init__.py
touch tests/python/gen/evgrpc/__init__.py
```

Output layout (subdir preserved from proto source path):

```
tests/python/gen/
├── __init__.py
└── evgrpc/
    ├── __init__.py
    ├── common_pb2.py            # common.proto has no `service`, so no common_pb2_grpc.py
    ├── charging_pb2.py
    ├── charging_pb2_grpc.py
    ├── consumption_pb2.py
    ├── consumption_pb2_grpc.py
    ├── display_pb2.py
    ├── display_pb2_grpc.py
    ├── source_category_pb2.py
    ├── source_category_pb2_grpc.py
    ├── vehicle_pb2.py
    ├── vehicle_pb2_grpc.py
    ├── weather_pb2.py
    └── weather_pb2_grpc.py
```

Imports inside test files:

```python
from tests.python.gen.evgrpc import vehicle_pb2 as pb
from tests.python.gen.evgrpc import vehicle_pb2_grpc as rpc
```

Stubs are **committed to git**. `scripts/gen_python_stubs.sh` is
run only when a proto changes (re-running it is idempotent — it
overwrites the committed files with byte-identical output if the
proto is unchanged).

### 5.5 Auth enforcement: `tests/python/test_auth_enforcement.py`

Uses its own bare channel (no metadata interceptor) so each test
can attach its own (or no) `authorization` metadata:

```python
import grpc, pytest
from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc

def _bare_channel():
    return grpc.insecure_channel("localhost:80")

def test_no_token_returns_unauthenticated():
    chan = _bare_channel()
    grpc.channel_ready_future(chan).result(timeout=5)
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(pb.SearchWeatherRequest())
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED

def test_malformed_token_returns_unauthenticated():
    chan = _bare_channel()
    grpc.channel_ready_future(chan).result(timeout=5)
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(
            pb.SearchWeatherRequest(),
            metadata=(("authorization", "Bearer not.a.real.jwt"),),
        )
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED

def test_forged_token_returns_unauthenticated():
    # Build a JWT with the real IdP's `iss`/`aud` claims but sign it
    # with a throwaway RSA key (generated per-test, never published).
    # evgrpc's JWT validator verifies signature against the IdP's
    # JWKS at auth-test.mksword.com, which does NOT contain this key,
    # so the signature check fails → UNAUTHENTICATED.
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives import serialization
    import jwt as pyjwt  # PyJWT
    ...
```

Forged-token test uses the `PyJWT` + `cryptography` libraries
(already in `py312`'s default toolchain via `pip install jwt`).
The JWT shape is built locally; signature is checked against the
real IdP's JWKS at request time and rejected. No IdP admin change
required.

### 5.6 Test documentation: `tests/python/doc/`

Per Goal 11, every test case must have a corresponding Markdown
section. One MD file per test file (not per test, not per RPC),
so 7 files total:

- `tests/python/doc/test_weather.md`
- `tests/python/doc/test_vehicle.md`
- `tests/python/doc/test_source_category.md`
- `tests/python/doc/test_charging.md`
- `tests/python/doc/test_consumption.md`
- `tests/python/doc/test_display.md`
- `tests/python/doc/test_auth_enforcement.md`

**Why one MD per test file (not per test):** 161 test cases ×
~30 lines each = ~5000 lines of MD if split per test. One MD per
test file with sections per test class (TestHappyPath /
TestErrorPath / TestBoundaries / TestConstraints) and
sub-sections per test keeps total doc volume to ~7 × ~150 =
~1000 lines, navigable in a single scroll, and groups related
tests so reviewers see context (e.g. all length-boundary tests
for `LicensePlate` next to each other). One MD per RPC was also
considered but splits the HappyPath / ErrorPath / Boundaries /
Constraints classification across files, losing the per-class
reviewability.

**Template** (one block per test):

```markdown
### test_<name>
- **RPC:** `Service.RpcName`
- **Purpose:** one sentence on what this test proves.
- **Setup:** prerequisite data/state (e.g. "create vehicle V1
  first" for UNIQUE-violation tests; "no setup" otherwise).
- **Action:** the gRPC call in plain English.
- **Expected:** the success response or `gRPC StatusCode.<NAME>`.
- **Cleanup:** which layer handles this row's DELETE
  (L1 TrackedInsert, L2a/L2b, or none for probe-only tests).
- **Rationale:** for boundary tests, why these specific values
  were chosen (e.g. "1/15 = ALLOWED (below/at limit), 16 =
  INVALID_ARGUMENT (over limit)"). For constraint tests, the
  schema clause that creates the constraint (e.g. "schema
  `sql/001_initial.sql` ~L12: `LicensePlate VARCHAR(15) NOT
  NULL UNIQUE`"). For happy/error tests, omit or note "obvious".
- **Related:** links to other tests covering adjacent surface
  (e.g. "see TestConstraints.test_create_vehicle_duplicate_...").
```

**Maintenance convention:** a new test may only be merged if its
MD section is added in the same commit as the test code. The
code-review gate enforces this manually in v1 (no automation);
§10 notes a future pytest plugin could enforce it via AST diff.

**Out of scope (auto-generation):** §10 explicitly rules out
auto-generating these MDs from test source. The point of the
docs is the human-written rationale (why 1/15/16, which schema
clause, which related test) — all of which auto-generators
strip or guess poorly. If a future contributor wants to add a
new boundary value (e.g. `license_plate` length = 14) they
should *think about why* the new value is interesting, and
writing the rationale by hand enforces that.

## 6. Data flow

### 6.1 Single test execution

```
pytest collects test_vehicle.py::TestHappyPath::test_create_vehicle_returns_id_and_brand
  ├─ conftest.session fixtures resolve:
  │     auth_token    → subprocess evgrpc-token → "eyJ..."  (~50ms cache hit)
  │     channel       → insecure_channel("localhost:80")
  │     namespace     → "7f3a2b1c"  (bare 8-hex)
  │     cleanup_namespace (autouse):
  │       PRE-yield: runs the children-first DELETE sweep (cleans any
  │                  prior orphans; idempotent if no orphans exist)
  │       yield
  ├─ function body:
  │     TrackedInsert("vehicle") yields
  │     stub = rpc.VehicleServiceStub(channel)
  │     req  = pb.CreateVehicleRequest(
  │                license_plate=make_license_plate("7f3a2b1c"),
  │                ...
  │            )
  │     resp = stub.CreateVehicle(req)             # gRPC call through nginx:80
  │     ti.register("vehicle", resp.id)
  │     assert resp.brand == req.brand
  ├─ TrackedInsert.__exit__: DELETE FROM vehicle WHERE id = '<resp.id>'
  ├─ (other tests run, each does its own TrackedInsert cycle)
  └─ session ends → cleanup_namespace post-yield runs:
        DELETE FROM consumption WHERE remark LIKE 'test-%'
        DELETE FROM charging    WHERE location LIKE 'test-%' OR remark LIKE 'test-%'
        DELETE FROM vehicle     WHERE license_plate LIKE 'test-%'
        DELETE FROM weather     WHERE name LIKE 'test-%'
        DELETE FROM source_category WHERE name LIKE 'test-%'
```

### 6.2 Server-unreachable handling

```
pytest collects first test
  ├─ conftest.session fixture `channel` resolves
  │     insecure_channel("localhost:80")
  │     grpc.channel_ready_future(chan).result(timeout=5)  # raises FutureTimeoutError
  │     pytest.skip("evgrpc:80 unreachable, skipping Python gRPC IT")
  └─ all tests in session skipped (count = 0, exit 0)
```

If `auth_token` fails first (e.g. IdP unreachable), the same skip
path applies with a different message.

### 6.3 Cleanup layers

| Layer | Trigger | Mechanism | Catches |
|---|---|---|---|
| L1 per-function | `TrackedInsert.__exit__` | `DELETE FROM <t> WHERE id = ANY(<ids>)` | clean exits + pytest exceptions (context manager always runs `__exit__`) |
| L2a session-start | `cleanup_namespace` pre-yield | `DELETE ... WHERE <col> LIKE 'test-%'` (children-first) | orphans from SIGKILL/OOM/power-loss in prior runs |
| L2b session-end | `cleanup_namespace` post-yield | same sweep, wrapped in `try/except Exception` so cleanup failures never mask test results | any rows this session forgot to clean up |

The pre-yield sweep happens before the first test runs, so even if
a prior session crashed mid-test, the next invocation sees a clean
slate. The post-yield sweep is best-effort (logged, not fatal).

### 6.4 Auth flow

```
test body calls stub.CreateVehicle(req)
  └─ grpc.Channel.interceptor adds metadata: ('authorization', f'Bearer <token>')
       └─ HTTP/2 frame to nginx:80
            └─ grpc_pass to evgrpc:50051 (internal Docker network)
                 └─ AuthServerInterceptor (src/auth/authenticate.cc)
                      ├─ missing header → UNAUTHENTICATED
                      ├─ bad token     → UNAUTHENTICATED
                      ├─ expired token → UNAUTHENTICATED
                      ├─ iss ≠ config.oauth.issuer_url → UNAUTHENTICATED
                      ├─ aud ≠ config.oauth.audience    → UNAUTHENTICATED
                      └─ signature fails verification   → UNAUTHENTICATED
```

`TestAuthEnforcement::test_*` makes unauthenticated or
attacker-signed calls (no metadata interceptor; explicit
`metadata=`) and asserts `UNAUTHENTICATED`.

## 7. File / directory layout

```
/data/Repositories/evGRpc/
├── proto/evgrpc/*.proto          # existing, source of truth
├── tests/python/                  # NEW
│   ├── __init__.py                # empty
│   ├── doc/                       # NEW (§5.6): per-test Markdown docs
│   ├── conftest.py                # session fixtures (§5.1)
│   ├── _helpers.py                # TrackedInsert + name builders (§5.2)
│   ├── test_weather.py            # per-service test files (§5.3)
│   ├── test_vehicle.py
│   ├── test_source_category.py
│   ├── test_charging.py
│   ├── test_consumption.py
│   ├── test_display.py
│   ├── test_auth_enforcement.py   # auth-layer tests (§5.5)
│   └── gen/                       # generated, committed (§5.4)
│       ├── __init__.py
│       └── evgrpc/
│           ├── __init__.py
│           ├── *_pb2.py
│           └── *_pb2_grpc.py
├── scripts/gen_python_stubs.sh    # NEW
├── environment.yml                # NEW
└── run_all_tests.sh               # modified: append Python stage
```

## 8. Test data isolation specifics

### 8.1 Namespace generation

`namespace` fixture returns `uuid.uuid4().hex[:8]` (8-char bare hex,
e.g. `7f3a2b1c`). Two parallel pytest runs (in different
terminals) get different prefixes and cannot collide. The
`test-` prefix is added by the `make_*` helpers in §5.2, never by
the namespace fixture.

### 8.2 Field-level application

All `make_*` outputs start with `test-` so the cleanup `LIKE
'test-%'` pattern matches across all tables.

| Table | Cleanup `LIKE` field(s) | Length budget |
|---|---|---|
| vehicle | `license_plate` | VARCHAR(15) — `make_license_plate` returns exactly 15 chars |
| weather | `name` | VARCHAR(36) — `make_weather_name` returns 29 chars |
| source_category | `name` | VARCHAR(36) — `make_source_category_name` returns 29 chars |
| charging | `location` OR `remark` | VARCHAR(100) / TEXT — `make_charging_location` returns 25 chars; tests may also populate `remark` |
| consumption | `remark` | `make_consumption_remark` returns 25 chars; tests must populate `Remark` to be cleaned by L2a/L2b sweep |

**Discipline**: tests that create `charging` rows must set
`Location` (and may set `Remark`); tests that create `consumption`
rows must set `Remark`. The `make_*` helpers exist precisely to
make this one-line. A linter rule or test-side assertion that
created-row helpers always return non-empty is out of scope for
v1; if a future bug shows up where a `charging` row's `Location` is
NULL, that's a test-writer omission and the post-yield sweep will
silently leave it (logged).

### 8.3 FK-cascade ordering (canonical, applies to both L2a and L2b)

Schema FKs are bare `REFERENCES vehicle(Id)` / `weather(Id)` /
`source_category(Id)` with **no `ON DELETE` clause** (verified
against `sql/001_initial.sql`). PostgreSQL default for unspecified
FK delete behavior is `NO ACTION`, which is functionally equivalent
to `RESTRICT` for the immediate constraint check: the parent row
cannot be deleted if any child row references it.

This makes **children-first** the only correct cleanup order:

1. `DELETE FROM consumption WHERE remark LIKE 'test-%'`
   — `consumption` depends on `vehicle` and `weather`.
2. `DELETE FROM charging WHERE location LIKE 'test-%' OR remark LIKE 'test-%'`
   — `charging` depends on `vehicle` and `source_category`.
3. `DELETE FROM vehicle WHERE license_plate LIKE 'test-%'`
   — only after both `consumption` and `charging` are emptied.
4. `DELETE FROM weather WHERE name LIKE 'test-%'`
   — only after `consumption` is emptied.
5. `DELETE FROM source_category WHERE name LIKE 'test-%'`
   — only after `charging` is emptied.

Parents-first order (the v1 §4 mistake) would fail at step 3 with
a `foreign_key_violation` from PG. The pre-yield sweep catches
this scenario and surfaces it as a pytest collection error if the
sweep itself raises — but the sweep ordering is now correct, so
this won't happen in practice.

### 8.4 Token cache (parallel runs)

`/tmp/evgrpc_token.json` is the default cache for `evgrpc-token`.
For parallel pytest invocations against the same dev box, set
`EVGRPC_CACHE` to a per-process path (e.g. via pytest's
`--env-files` or a `conftest.py` shim that derives from `os.getpid`)
to avoid concurrent refresh races. The default shared cache is
fine for the typical single-process pytest run.

## 9. Run / CI integration

### 9.1 Local dev

```bash
cd /data/Repositories/evGRpc
conda env create -f environment.yml          # one-time
conda activate evgrpc-tests
pytest tests/python/ -v                      # full run, ~105s
pytest tests/python/test_vehicle.py -v       # single file
pytest tests/python/ -v -k boundaries        # by class
pytest tests/python/ -v --tb=long            # for failure debugging
```

If a `.proto` file changes:

```bash
bash scripts/gen_python_stubs.sh
git add tests/python/gen/ && git commit
```

### 9.2 `run_all_tests.sh` addition

Append to the existing script:

```bash
echo "=== Python gRPC IT ==="
conda run -n evgrpc-tests pytest tests/python/ --tb=short -q || exit 1
```

### 9.3 `environment.yml`

```yaml
name: evgrpc-tests
channels:
  - conda-forge
dependencies:
  - python=3.12
  - pip
  - pytest>=8.0
  - protobuf>=5.0
  - pip:
      - grpcio>=1.60
      - grpcio-tools>=1.60
      - psycopg[binary]>=3.1
      - pyjwt>=2.8
      - cryptography>=42.0
```

Notes:
- `grpcio` is installed via pip (not conda-forge) because the
  conda-forge build lags and has historically had abseil-cpp
  compatibility issues. Same for `psycopg[binary]` (conda-forge's
  `psycopg` lacks the `[binary]` extras — the bracketed form is
  pip-extras syntax).
- `pyjwt` and `cryptography` are needed for the forged-token
  auth test (§5.5).

## 10. Out of scope / future work

- **Auto-generated test docs.** §5.6 rules this out: the docs
  are for human-written rationale, which auto-generators strip.

- **RBAC / per-RPC scope enforcement.** Add when server grows scope
  checking; the test harness already supports `metadata=` overrides
  per call.
- **Coverage report (`pytest-cov`).** Not requested; deferred.
- **Parallel execution (`pytest-xdist`).** With 161 tests × 0.65s
  ≈ 105s serial, parallelism adds setup complexity for marginal
  speed. Revisit if runtime becomes a problem.
- **CI workflow file (`github-actions.yml` or similar).** Out of
  scope; the user runs tests locally via `run_all_tests.sh`.
- **Pyright / mypy static analysis.** Out of scope; pytest + runtime
  checks are the v1 bar.
- **Auto-start of `docker compose`.** Users run docker-compose
  manually before pytest.
- **Expired-token test.** Requires a dedicated IdP client with
  short token lifetime (admin UI change). Add when a second
  use case for short-lived tokens appears.

## 11. Open questions

None — all design decisions resolved during brainstorming.

## 12. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| `evgrpc-token` subprocess hangs | Low | 15s timeout via `subprocess.run(..., timeout=15)` |
| `evgrpc-token` subprocess fails (IdP down) | Low | `pytest.skip` with clear message — same as channel fixture |
| docker-compose stack down mid-test | Low | Session-skip on `grpc.FutureTimeoutError` |
| Test pollutes dev DB despite prefix | Low | Triple-layer cleanup (L1 function + L2a session-start + L2b session-end) |
| Orphan rows from SIGKILL/OOM | Low | Session-start sweep catches on next pytest invocation |
| Generated stubs drift from proto | Medium | `scripts/gen_python_stubs.sh` always re-runnable; pre-commit hook (out of scope) would gate |
| New RPC added without test | Low (v1) | Per-RPC checklist during code review; deferred to a future "spec gate" workflow |
| `psycopg` direct-DB access for cleanup | Low | Acceptable per spec §4 — test code, test DB |
| Token cache race in parallel pytest | Low | `EVGRPC_CACHE` env var documented for per-process isolation |
| Test forgets to set `Remark` on consumption | Low | L2 sweep silently leaves the row; logged in post-yield report |