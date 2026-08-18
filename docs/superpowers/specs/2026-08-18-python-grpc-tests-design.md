# Python gRPC Integration Test Suite for Deployed evGRpc

- **Date:** 2026-08-18
- **Status:** Design (awaiting review)
- **Replaces:** none
- **Supersedes:** none

## 0. Revision History

- **v1 (initial):** First draft after brainstorming session.

## 1. Background

`evGRpc` ships 6 gRPC services with **28 RPCs** total. The C++ test
suite (`tests/unit/`, `tests/integration/`, `tests/integration/smoke_e2e_test.cc`)
covers the service layer end-to-end against a real Postgres + an
in-process gRPC server. It is fast, thorough on the C++ side, and runs
under `ctest`. What's missing is a **client-side integration test
suite written in Python**:

1. The C++ suite boots its own `TestServer` (in-process gRPC + JWKS
   HTTP + bypass auth) inside the test binary. It does **not** exercise
   the `nginx → evgrpc → Postgres` stack that production traffic flows
   through. Bugs that only show up at the deployment boundary — wrong
   gRPC framing through nginx, HTTP/2 negotiation, OIDC bearer-token
   rejection, response size limits — are invisible to the C++ suite.
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

1. **All 28 RPCs have at least one happy-path test** exercised through
   `localhost:80` with a real Bearer token from
   `https://auth-test.mksword.com/` (via the existing `evgrpc-token`
   helper).
2. **All 28 RPCs have at least one error-path test** (NOT_FOUND,
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
6. **Auth enforcement tests** at the gRPC layer: no token → 401;
   expired token → 401; bogus token → 401. (Authorization-scope /
   RBAC tests deferred — see §3.)
7. **Total runtime ≤ 90s** on the dev VM for the full suite
   (~150 cases × ≤ 0.6 s/case). Hard-fail threshold is `> 120s`.
8. **No pollution of dev data**: tests insert rows only with a unique
   `test-<uuid>-` prefix and clean up at function + session teardown.
9. **One-command setup**: `conda env create -f environment.yml &&
   conda activate evgrpc-tests && bash scripts/gen_python_stubs.sh &&
   pytest tests/python/ -v` works on a fresh clone.
10. The suite **slots into `run_all_tests.sh`** as a final stage that
    fails the script non-zero on any pytest failure.

## 3. Non-Goals

- RBAC / scope-claim enforcement (`evgrpc` does not currently
  enforce per-RPC scopes; the OIDC validator only checks iss/aud/exp).
  When the server grows scope checking, this suite should be extended
  — but not in this spec.
- Branch-coverage reporting. Line coverage is not even a goal (pytest
  doesn't natively cover Python client code with the same rigor as
  gtest on C++). Pass/fail is the success metric.
- Performance benchmarks, fuzz tests, property-based tests.
- Mock-gRPC-server unit tests. The whole point is to hit the real
  deployed service. Pure unit tests of the Python client wrapper
  belong in a separate (future) spec.
- Re-enabling testcontainers-cpp / ephemeral Postgres. The suite uses
  the existing `evgrpc` database via the existing docker-compose stack.
- Migration of the existing C++ integration suite. That suite stays
  as-is and continues to run via `ctest`. The new Python suite is
  additive.
- Auto-start of `docker compose up`. The Python suite expects
  `localhost:80` to already be serving. If it isn't, the session is
  skipped with a clear message (see §6.4).

## 4. Architecture

```
pytest tests/python/ -v
  └─ Session scope (one process):
       ├─ conftest.py::auth_token       # subprocess evgrpc-token, cached
       ├─ conftest.py::channel          # insecure localhost:80, bearer interceptor
       │     - fails fast if server unreachable → pytest.skip
       ├─ conftest.py::namespace        # test-<uuid>-  prefix (session-unique)
       └─ conftest.py::cleanup_namespace (autouse, session scope)
             - DELETE FROM vehicle WHERE license_plate LIKE 'test-<ns>%'
             - DELETE FROM weather WHERE name LIKE 'test-<ns>%'
             - DELETE FROM source_category WHERE name LIKE 'test-<ns>%'
             - DELETE FROM charging WHERE ... LIKE 'test-<ns>%'
             - DELETE FROM consumption WHERE ... LIKE 'test-<ns>%'
  └─ Function scope (per test):
       ├─ _helpers.py::truncate_my_rows()  # tracks rows inserted, DELETEs on teardown
       └─ _helpers.py::make_<entity>()     # builds request proto with test-<ns> fields
  └─ Test file per service:
       test_weather.py
       test_vehicle.py
       test_source_category.py
       test_charging.py
       test_consumption.py
       test_display.py
       └─ TestHappyPath
       └─ TestErrorPath
       └─ TestBoundaries
       └─ TestConstraints
```

The suite is **one process, one channel, one namespace per
`pytest` invocation**. Tests run serially (`pytest-xdist` is not
required). Each test inserts data with the session-unique
`test-<ns>-` prefix and tears down its own rows on function exit.
The session-end fixture also wipes anything that escaped (defense in
depth).

## 5. Components

### 5.1 `tests/python/conftest.py`

Defines four session-scoped fixtures:

- **`auth_token`** — runs `subprocess.check_output(["evgrpc-token"])`
  once at session start. Reused across all tests. When the cached
  token expires (`> 50min` from issue), re-runs. The cache file at
  `/tmp/evgrpc_token.json` is shared with the CLI helper, so
  interactive use stays in sync.
- **`channel`** — creates an insecure gRPC channel to `localhost:80`,
  installs a metadata-injecting interceptor that adds
  `authorization: Bearer <auth_token>` to every call. Validates the
  channel with `grpc.channel_ready_future(...).result(timeout=5)`.
  If the timeout elapses, raises a session-skip via
  `pytest.skip("evgrpc:80 unreachable, skipping Python gRPC IT")`.
- **`namespace`** — generates `test-<8-char-uuid>-` once per session.
  Used by every test as a prefix on all generated identifiers
  (`license_plate`, `weather.name`, `source_category.name`,
  `Location` strings).
- **`cleanup_namespace`** — `autouse=True`, `scope="session"`. After
  `yield`, opens a `psycopg` connection to `evgrpc` DB and runs the
  `DELETE ... LIKE 'test-<ns>%'` cleanup listed in §4. Logs how many
  rows per table were deleted.

### 5.2 `tests/python/_helpers.py`

- **`class TrackedInsert`** — context manager that yields a closure
  to register rows the test inserted (`register(table, id)`), then
  on `__exit__` runs `DELETE FROM <table> WHERE id = ANY(<ids>)`.
  Used by function-level teardown for the per-test cleanup layer
  (C3 first half).
- **`make_license_plate(ns)`** — returns `f"test-{ns}{uuid4()}"`,
  length ≤ 15 chars (VARCHAR(15) limit).
- **`make_weather_name(ns)`** — returns `f"test-{ns}{uuid4()}"`,
  length ≤ 36 chars (VARCHAR(36) limit).
- **`make_uuid()`** — returns `str(uuid.uuid4())` (for `Vehicle.Id`).
- **`mint_grpc_channel(addr, token)`** — factory used by `channel`
  fixture; also exported for any test that needs a one-off channel
  (none expected in v1).

### 5.3 Per-service test files

Each follows the same shape:

```python
# test_vehicle.py
import grpc, pytest
from tests.python.gen import evgrpc_pb2 as pb
from tests.python.gen import evgrpc_pb2_grpc as rpc
from tests.python._helpers import TrackedInsert, make_license_plate, make_uuid

class TestHappyPath:
    def test_create_vehicle_returns_id_and_brand(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        req = pb.CreateVehicleRequest(
            brand=f"test-{namespace}brand",
            calibrated_range_km=400,
            battery_capacity_kwh=75.0,
            purchase_date=...,
            license_plate=make_license_plate(namespace),
        )
        with TrackedInsert("vehicle", stub=stub) as ti:
            resp = stub.CreateVehicle(req)
            ti.register("vehicle", resp.id)
        assert resp.brand == req.brand
        ...

class TestErrorPath:
    def test_get_vehicle_unknown_id_returns_not_found(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicle(pb.GetVehicleRequest(id=str(uuid.uuid4())))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

class TestBoundaries:
    @pytest.mark.parametrize("plate_len", [1, 15, 16])
    def test_create_vehicle_license_plate_length(self, channel, namespace, plate_len):
        # 1 and 15 → ALLOWED; 16 → INVALID_ARGUMENT
        ...

class TestConstraints:
    def test_create_vehicle_duplicate_license_plate_returns_already_exists(self, ...):
        ...
```

Per-service test count estimate (sum = 156):

| Service | RPCs | Happy | Error | Boundaries | Constraints | Total |
|---|---:|---:|---:|---:|---:|---:|
| WeatherService | 2 | 2 | 2 | 6 | 2 | 12 |
| VehicleService | 5 | 6 | 5 | 12 | 4 | 27 |
| SourceCategoryService | 2 | 2 | 2 | 4 | 2 | 10 |
| ChargingService | 5 | 6 | 5 | 16 | 8 | 35 |
| ConsumptionService | 5 | 6 | 5 | 14 | 6 | 31 |
| DisplayService | 11 | 12 | 11 | 14 | 6 | 43 |
| (Auth enforcement) | — | — | 3 | — | — | 3 |
| **Total** | **28** | **34** | **33** | **66** | **28** | **161** |

Numbers above are estimates. The actual count will land within ±10%
once tests are written.

### 5.4 Stub generation: `scripts/gen_python_stubs.sh`

```bash
#!/usr/bin/env bash
# Regenerate Python gRPC stubs from proto/*.proto.
# Re-run whenever a .proto file changes.
set -euo pipefail
cd "$(dirname "$0")/.."
python -m grpc_tools.protoc \
    -I proto \
    --python_out=tests/python/gen \
    --grpc_python_out=tests/python/gen \
    proto/evgrpc/*.proto
# generated *_pb2_grpc.py uses `import evgrpc_pb2`; rewrite to relative import
sed -i 's/^import evgrpc_pb2 as /from . import evgrpc_pb2 as /' \
    tests/python/gen/evgrpc_pb2_grpc.py
```

The generated stubs are **committed to git** so a fresh clone needs
only `conda env create + activate + pytest`, no `protoc` toolchain.

## 6. Data flow

### 6.1 Single test execution

```
pytest collects test_vehicle.py::TestHappyPath::test_create_vehicle_returns_id_and_brand
  ├─ conftest.session fixtures resolve:
  │     auth_token    → subprocess evgrpc-token → "eyJ..."  (50ms cache hit)
  │     channel       → insecure_channel("localhost:80")
  │     namespace     → "test-7f3a2b1c-"
  │     cleanup_namespace (autouse) → yield
  ├─ function fixture resolves:
  │     TrackedInsert("vehicle") yields
  ├─ test body:
  │     stub = rpc.VehicleServiceStub(channel)
  │     req  = pb.CreateVehicleRequest(license_plate=f"test-{ns}{uuid}")
  │     resp = stub.CreateVehicle(req)             # gRPC call through nginx:80
  │     ti.register("vehicle", resp.id)
  ├─ test asserts pass
  ├─ TrackedInsert.__exit__: DELETE FROM vehicle WHERE id = '<resp.id>'
  └─ (other tests run…)
  └─ last test finishes → cleanup_namespace post-yield runs:
        DELETE FROM vehicle WHERE license_plate LIKE 'test-7f3a2b1c-%'
        DELETE FROM weather WHERE name LIKE 'test-7f3a2b1c-%'
        ... (per table)
```

### 6.2 Server-unreachable handling

```
pytest collects first test
  ├─ conftest.session fixture `channel` resolves
  │     insecure_channel("localhost:80")
  │     grpc.channel_ready_future(chan).result(timeout=5)  # raises DeadlineExceeded
  │     pytest.skip("evgrpc:80 unreachable, skipping Python gRPC IT")
  └─ all tests in session skipped (count = 0, exit 0)
```

### 6.3 Cleanup layers (C3)

| Layer | Trigger | Mechanism | Failure mode |
|---|---|---|---|
| L1 per-function | `TrackedInsert.__exit__` | `DELETE FROM <t> WHERE id = ANY(<ids>)` | If exception in test body, still runs in `__exit__` (context manager) |
| L2 per-session | `cleanup_namespace` post-yield | `DELETE FROM <t> WHERE <idcol> LIKE 'test-<ns>%'` (via `psycopg`) | Catches anything L1 missed (e.g. test crash before L1) |

If the dev DB is somehow polluted from a prior broken run, the
session-end DELETE also catches it as long as the prefix is the same.

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

Tests `TestAuthEnforcement::test_*` make unauthenticated calls (no
metadata interceptor applied) and assert `UNAUTHENTICATED`.

## 7. File / directory layout

```
/data/Repositories/evGRpc/
├── proto/evgrpc/*.proto          # existing, source of truth
├── tests/python/                  # NEW
│   ├── __init__.py                # empty
│   ├── conftest.py                # session fixtures (5.1)
│   ├── _helpers.py                # TrackedInsert + name builders (5.2)
│   ├── test_weather.py            # per-service test files (5.3)
│   ├── test_vehicle.py
│   ├── test_source_category.py
│   ├── test_charging.py
│   ├── test_consumption.py
│   ├── test_display.py
│   ├── test_auth_enforcement.py   # auth-layer tests (§6.4)
│   └── gen/                       # generated, committed
│       ├── __init__.py
│       ├── evgrpc_pb2.py
│       └── evgrpc_pb2_grpc.py
├── scripts/gen_python_stubs.sh    # NEW
├── environment.yml                # NEW
└── run_all_tests.sh               # modified: append Python stage
```

## 8. Test data isolation specifics

### 8.1 Namespace generation

`namespace` fixture returns `test-{uuid.uuid4().hex[:8]}-`. Example:
`test-7f3a2b1c-`. Two parallel pytest runs (in different terminals)
will have different prefixes and not collide.

### 8.2 Field-level application

| Table | Field used for namespace prefix | Cleanup `LIKE` clause |
|---|---|---|
| vehicle | `LicensePlate` | `WHERE license_plate LIKE 'test-<ns>%'` |
| weather | `Name` | `WHERE name LIKE 'test-<ns>%'` |
| source_category | `Name` | `WHERE name LIKE 'test-<ns>%'` |
| charging | `Location` (or `Remark`) | `WHERE location LIKE 'test-<ns>%' OR remark LIKE 'test-<ns>%'` |
| consumption | `Remark` | `WHERE remark LIKE 'test-<ns>%'` |

For tables whose primary identifier (UUID) is server-generated and
not human-readable, we use the **only** human-readable VARCHAR column
available. This is why the cleanup `LIKE` clause is on `license_plate`,
`name`, `location`, `remark` — not on `Id`.

### 8.3 FK-cascade risk

If a test inserts a `vehicle` with a `test-<ns>-` `license_plate`,
and a subsequent test inserts a `charging` row referencing that
vehicle, then the per-session cleanup deletes `vehicle` rows first
→ cascade deletes `charging` → `cleanup_namespace` then tries to
`DELETE FROM charging WHERE location LIKE 'test-<ns>%'` and finds 0
rows. Harmless.

To keep cleanup deterministic, the order is:
1. `DELETE FROM consumption WHERE ...` (depends on vehicle, weather)
2. `DELETE FROM charging WHERE ...` (depends on vehicle, source_category)
3. `DELETE FROM vehicle WHERE ...`
4. `DELETE FROM weather WHERE ...`
5. `DELETE FROM source_category WHERE ...`

This handles FK cascades cleanly.

## 9. Run / CI integration

### 9.1 Local dev

```bash
cd /data/Repositories/evGRpc
conda env create -f environment.yml          # one-time
conda activate evgrpc-tests
bash scripts/gen_python_stubs.sh             # only if proto changed
pytest tests/python/ -v                      # full run, ~60s
pytest tests/python/test_vehicle.py -v       # single file
pytest tests/python/ -v -k boundaries        # by class
pytest tests/python/ -v --tb=long            # for failure debugging
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
  - psycopg[binary]>=3.1
  - pip:
      - grpcio>=1.60
      - grpcio-tools>=1.60
```

Note: `grpcio` is installed via pip (not conda-forge) because the
conda-forge build lags and has historically had abseil-cpp
compatibility issues.

## 10. Out of scope / future work

- **RBAC / per-RPC scope enforcement.** Add when server grows scope
  checking; the test harness already supports `metadata=` overrides
  per call via the channel interceptor.
- **Coverage report (`pytest-cov`).** Not requested; deferred.
- **Parallel execution (`pytest-xdist`).** With ~150 tests × 0.6s =
  90s serial, parallelism adds setup complexity for marginal speed.
  Revisit if runtime becomes a problem.
- **CI workflow file (`github-actions.yml` or similar).** Out of
  scope for this spec; the user runs tests locally via
  `run_all_tests.sh`.
- **Pyright / mypy static analysis.** Out of scope; pytest + runtime
  checks are the v1 bar.
- **Auto-start of `docker compose`.** Users run docker-compose
  manually before pytest.

## 11. Open questions

None — all design decisions resolved during brainstorming.

## 12. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| `evgrpc-token` subprocess hangs | Low | 15s timeout via `subprocess.run(..., timeout=15)`; cache fallback |
| docker-compose stack down mid-test | Low | Session-skip on `channel_ready_future` timeout |
| Test pollutes dev DB despite prefix | Low | Double-layer cleanup (function + session); rollback TRUNCATE if needed |
| Generated stubs drift from proto | Medium | `scripts/gen_python_stubs.sh` always re-runnable; CI gate to check |
| New RPC added without test | Low (v1) | Per-RPC checklist; deferred to a future "spec gate" workflow |
| `psycopg` direct-DB access for cleanup | Low | Acceptable per spec §4 — test code, test DB |
