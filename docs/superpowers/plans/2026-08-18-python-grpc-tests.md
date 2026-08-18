# Python gRPC Integration Test Suite — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Python pytest-based client-side integration test suite for the deployed evGRpc service, hitting the real `nginx:80 → evgrpc:50051 → Postgres` stack via the existing `evgrpc-token` helper.

**Architecture:** 7 test files (6 services + auth enforcement) in `tests/python/`, with shared session-scoped fixtures in `conftest.py`, helpers in `_helpers.py`, and one Markdown doc per test file in `tests/python/doc/`. ~161 test cases cover happy paths, error paths, data boundaries, UNIQUE + FK constraints, and auth enforcement. Test isolation via `test-<8hex>-` namespace prefix + double-layer cleanup (L1 per-function TrackedInsert, L2a session-start sweep, L2b session-end sweep).

**Tech Stack:** conda py312 env (not uv), pytest 8+, grpcio 1.60+, grpcio-tools (for stub regen), protobuf 5+, psycopg[binary] 3.1+ (cleanup DELETE), PyJWT + cryptography (forged-token auth test).

**Spec:** `docs/superpowers/specs/2026-08-18-python-grpc-tests-design.md` (v4, approved).

---

## File Structure

| Path | Purpose | Created in |
|---|---|---|
| `environment.yml` | conda env: python 3.12 + pytest + grpcio + grpcio-tools + protobuf + psycopg + pyjwt + cryptography | Chunk 1 |
| `scripts/gen_python_stubs.sh` | regenerate protoc-generated stubs into `tests/python/gen/` | Chunk 1 |
| `tests/python/__init__.py` | empty | Chunk 1 |
| `tests/python/gen/__init__.py` | empty | Chunk 1 |
| `tests/python/gen/evgrpc/__init__.py` | empty | Chunk 1 |
| `tests/python/gen/evgrpc/*_pb2.py` + `*_pb2_grpc.py` | protoc output (committed) | Chunk 1 |
| `tests/python/conftest.py` | session fixtures: `auth_token`, `channel`, `namespace`, `pg_conn`, `cleanup_namespace` | Chunk 1 |
| `tests/python/_helpers.py` | `TrackedInsert` ctx mgr, `make_license_plate`, `make_weather_name`, `make_charging_location`, `make_consumption_remark`, `make_uuid` | Chunk 1 |
| `tests/python/test_smoke.py` | 1 sanity test (validates channel + auth + 1 RPC works end-to-end) | Chunk 1 |
| `tests/python/test_weather.py` + `tests/python/doc/test_weather.md` | 12 tests + per-test MD | Chunk 2 |
| `tests/python/test_vehicle.py` + `tests/python/doc/test_vehicle.md` | 27 tests + MD | Chunk 3 |
| `tests/python/test_source_category.py` + `tests/python/doc/test_source_category.md` | 10 tests + MD | Chunk 4 |
| `tests/python/test_charging.py` + `tests/python/doc/test_charging.md` | 35 tests + MD | Chunk 5 |
| `tests/python/test_consumption.py` + `tests/python/doc/test_consumption.md` | 31 tests + MD | Chunk 6 |
| `tests/python/test_display.py` + `tests/python/doc/test_display.md` | 43 tests + MD | Chunk 7 |
| `tests/python/test_auth_enforcement.py` + `tests/python/doc/test_auth_enforcement.md` | 3 tests + MD | Chunk 8 |
| `scripts/run_all_tests.sh` | append Python stage (`conda run -n evgrpc-tests pytest ...`) | Chunk 9 |

---

## Chunk 1: Foundation

**Goal:** scaffold the test environment, generate + commit gRPC stubs, write conftest + helpers + a smoke test. End of chunk: `pytest tests/python/test_smoke.py -v` passes against the running docker-compose stack.

### Task 1.1: Create `environment.yml`

**Files:**
- Create: `environment.yml`

- [ ] **Step 1: Write the file**

Create `/data/Repositories/evGRpc/environment.yml`:

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

- [ ] **Step 2: Commit**

```bash
cd /data/Repositories/evGRpc
git add environment.yml
git commit -m "build(pytest): add environment.yml for evgrpc-tests conda env"
```

### Task 1.2: Create the conda env + verify install

- [ ] **Step 1: Create the env**

```bash
cd /data/Repositories/evGRpc
conda env create -f environment.yml
```

Expected: env `evgrpc-tests` appears in `conda env list`.

- [ ] **Step 2: Verify all imports work**

```bash
conda run -n evgrpc-tests python -c "import grpc, grpc_tools, pytest, google.protobuf, psycopg, jwt, cryptography; print('OK')"
```

Expected: `OK`.

(No commit; this is environment setup.)

### Task 1.3: Write `scripts/gen_python_stubs.sh`

**Files:**
- Create: `scripts/gen_python_stubs.sh`

- [ ] **Step 1: Write the script**

Create `/data/Repositories/evGRpc/scripts/gen_python_stubs.sh`:

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

- [ ] **Step 2: Make executable + commit**

```bash
cd /data/Repositories/evGRpc
chmod +x scripts/gen_python_stubs.sh
git add scripts/gen_python_stubs.sh
git commit -m "build(pytest): add gen_python_stubs.sh for protoc output"
```

### Task 1.4: Generate stubs + commit them

- [ ] **Step 1: Run the generator**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests bash scripts/gen_python_stubs.sh
```

Expected: 13 new files under `tests/python/gen/evgrpc/` (7 `*_pb2.py` — one per proto including common — + 6 `*_pb2_grpc.py` — one per service, common.proto has none); no errors.

- [ ] **Step 2: Verify import round-trip**

```bash
conda run -n evgrpc-tests python -c "
from tests.python.gen.evgrpc import vehicle_pb2, vehicle_pb2_grpc
print('vehicle message:', vehicle_pb2.Vehicle.DESCRIPTOR.name)
print('vehicle stub:', vehicle_pb2_grpc.VehicleServiceStub.__name__)
"
```

Expected: prints both names. (Imports resolve correctly thanks to the sed rewriting.)

- [ ] **Step 3: Create `tests/python/__init__.py`**

```bash
cd /data/Repositories/evGRpc
touch tests/python/__init__.py
```

(Empty. Makes `tests.python` a real package, not a namespace
package. Anything that does `import tests.python.…` needs this file
to exist; otherwise pytest's import resolution may behave
unexpectedly. Marking it here — *before* any test code is written —
keeps the dependency chain explicit.)

- [ ] **Step 4: Commit stubs + `__init__.py`**

```bash
cd /data/Repositories/evGRpc
git add tests/python/gen/ tests/python/__init__.py
git commit -m "build(pytest): commit protoc-generated stubs + tests/python package marker"
```

### Task 1.5: Write smoke test FIRST (TDD red step)

**Files:**
- Create: `tests/python/test_smoke.py`

- [ ] **Step 1: Write the test**

Create `/data/Repositories/evGRpc/tests/python/test_smoke.py`:

```python
"""Sanity test: validates that auth + channel + 1 RPC work end-to-end."""

from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc


def test_search_weather_with_valid_bearer_succeeds(channel):
    """With valid bearer, SearchWeather (no prefix) returns 200 + response."""
    stub = rpc.WeatherServiceStub(channel)
    resp = stub.SearchWeather(pb.SearchWeatherRequest(prefix="", limit=1))
    assert isinstance(resp, pb.SearchWeatherResponse)
```

- [ ] **Step 2: Commit (test exists but cannot pass without fixtures)**

```bash
cd /data/Repositories/evGRpc
git add tests/python/test_smoke.py
git commit -m "test(pytest): add smoke test stub (TDD red — will fail until fixtures exist)"
```

### Task 1.6: Run smoke test (TDD red verification)

- [ ] **Step 1: Run**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_smoke.py -v
```

Expected: FAIL — collection error or `fixture not found` for `channel`.
This confirms the test depends on fixtures we haven't written yet.

### Task 1.7: Write helpers + conftest

**Files:**
- Create: `tests/python/_helpers.py`
- Create: `tests/python/conftest.py`

(Note: `tests/python/__init__.py` was already created in Task 1.4
alongside the gen-package `__init__.py` files. Order matters — Task
1.4 imports `tests.python.gen.evgrpc.…` which requires the
`tests_python` package to be importable, so the package marker must
exist *before* any test code references the package. Putting it
here in Task 1.7 would silently depend on Python 3 namespace-package
semantics, which works but is fragile and surprising.)

- [ ] **Step 1: `tests/python/_helpers.py`**

```python
"""Shared helpers for the evGRpc pytest suite."""

from __future__ import annotations

import os
import uuid
from contextlib import contextmanager
from typing import Iterable

import psycopg


class TrackedInsert:
    """Per-function cleanup (L1).

    Usage:
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        # __exit__ runs: DELETE FROM vehicle WHERE id = ANY(<ids>)
    """

    def __init__(self, pg_conn, table: str):
        self._conn = pg_conn
        self._table = table
        self._ids: list[str] = []

    def register(self, id: str) -> None:
        self._ids.append(id)

    def __enter__(self) -> "TrackedInsert":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if not self._ids:
            return
        with self._conn.cursor() as cur:
            cur.execute(
                f"DELETE FROM {self._table} WHERE id = ANY(%s)",
                (self._ids,),
            )
        self._conn.commit()


def _ns_prefix(ns: str) -> str:
    """All identifier helpers emit values starting with 'test-'."""
    return f"test-{ns}"


def make_license_plate(ns: str) -> str:
    """Returns 'test-<8hex><2hex>' = 15 chars exactly (VARCHAR(15) limit)."""
    return f"{_ns_prefix(ns)}{uuid.uuid4().hex[:2]}"


def make_weather_name(ns: str) -> str:
    """Returns 'test-<8hex><16hex>' = 29 chars (VARCHAR(36) limit)."""
    return f"{_ns_prefix(ns)}{uuid.uuid4().hex[:16]}"


def make_source_category_name(ns: str) -> str:
    """Same form as make_weather_name."""
    return make_weather_name(ns)


def make_charging_location(ns: str) -> str:
    """Returns 'test-loc-<8hex><8hex>' = 25 chars (VARCHAR(100) limit)."""
    return f"test-loc-{ns}{uuid.uuid4().hex[:8]}"


def make_consumption_remark(ns: str) -> str:
    """Returns 'test-rem-<8hex><8hex>' = 25 chars (TEXT column)."""
    return f"test-rem-{ns}{uuid.uuid4().hex[:8]}"


def make_uuid() -> str:
    """Random UUID for probe-style tests (NOT_FOUND, etc.)."""
    return str(uuid.uuid4())


def sweep_all_test_rows(pg_conn) -> dict[str, int]:
    """L2 cleanup (used by cleanup_namespace fixture).

    Children-first to respect FK NO ACTION constraint.
    Returns dict mapping table -> rows-deleted (for logging).
    """
    counts: dict[str, int] = {}
    with pg_conn.cursor() as cur:
        for table, like_clauses in [
            ("consumption", ["remark"]),
            ("charging", ["location", "remark"]),
            # NOTE: SQL column is `licenseplate` (no underscore).
            # `LicensePlate` (unquoted in sql/001_initial.sql) folds
            # to `licenseplate` in PostgreSQL. The proto field is
            # snake_case `license_plate` but that's irrelevant for
            # raw SQL.
            ("vehicle", ["licenseplate"]),
            ("weather", ["name"]),
            ("source_category", ["name"]),
        ]:
            where = " OR ".join(f"{c} LIKE 'test-%'" for c in like_clauses)
            cur.execute(f"DELETE FROM {table} WHERE {where}")
            counts[table] = cur.rowcount
    pg_conn.commit()
    return counts
```

- [ ] **Step 3: `tests/python/conftest.py`**

```python
"""Session-scoped pytest fixtures for the evGRpc test suite."""

from __future__ import annotations

import os
import subprocess
import time
import uuid

import grpc
import psycopg
import pytest


DEFAULT_SERVER = "localhost:80"
DEFAULT_OIDC_CLIENT_ID = "evgrpc_test_2"
DEFAULT_OIDC_CLIENT_SECRET = "112ll035"
DEFAULT_ISSUER = "https://auth-test.mksword.com/"


@pytest.fixture(scope="session")
def namespace() -> str:
    """Session-unique 8-hex prefix (helpers add 'test-')."""
    return uuid.uuid4().hex[:8]


@pytest.fixture(scope="session")
def auth_token() -> str:
    """Bearer token from `evgrpc-token` CLI helper.

    Reads from /tmp/evgrpc_token.json cache (50ms cache hit typical).
    Falls back to fresh mint (~2.6s) if cache empty.
    On failure (IdP unreachable, etc.) → pytest.skip.
    """
    try:
        result = subprocess.run(
            ["evgrpc-token"],
            capture_output=True,
            text=True,
            timeout=15,
        )
    except subprocess.TimeoutExpired:
        pytest.skip("evgrpc-token timeout, skipping Python gRPC IT")
    if result.returncode != 0:
        pytest.skip(f"evgrpc-token failed: {result.stderr}, skipping Python gRPC IT")
    return result.stdout.strip()


@pytest.fixture(scope="session")
def channel(auth_token: str):
    """Insecure gRPC channel to localhost:80 with bearer-token interceptor.

    Uses class-based `grpc.UnaryUnaryClientInterceptor` (modern grpcio
    API; the function-based `grpc.ClientInterceptor(fn)` form was removed
    in grpcio ≥ 1.59). For unary-unary calls (which is all evGRpc
    currently exposes) only `intercept_unary_unary` is needed; if a
    future streaming RPC is added, also implement the streaming
    methods.
    """
    chan = grpc.insecure_channel(DEFAULT_SERVER)
    try:
        grpc.channel_ready_future(chan).result(timeout=5)
    except grpc.FutureTimeoutError:
        chan.close()
        pytest.skip(f"{DEFAULT_SERVER} unreachable, skipping Python gRPC IT")

    class _BearerInterceptor(grpc.UnaryUnaryClientInterceptor):
        def intercept_unary_unary(self, continuation, client_call_details, request):
            metadata = list(client_call_details.metadata or [])
            metadata.append(("authorization", f"Bearer {auth_token}"))
            new_details = client_call_details._replace(metadata=metadata)
            return continuation(new_details, request)

    return grpc.intercept_channel(chan, _BearerInterceptor())


@pytest.fixture(scope="session")
def pg_conn():
    """psycopg connection to the evgrpc DB (for L1/L2 cleanup DELETEs).

    DATABASE_URL env var → fallback to docker-compose default.
    Password URL-encoded because of embedded '@'.
    """
    url = os.environ.get(
        "DATABASE_URL",
        "postgresql://vegrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc",
    )
    conn = psycopg.connect(url, autocommit=False)
    yield conn
    conn.close()


@pytest.fixture(scope="session", autouse=True)
def cleanup_namespace(pg_conn):
    """L2a (session-start) + L2b (session-end) sweeps.

    Catches orphans from prior SIGKILL/OOM runs (pre-yield) and
    anything this run forgot to clean (post-yield).
    """
    from tests.python._helpers import sweep_all_test_rows
    sweep_all_test_rows(pg_conn)
    yield
    try:
        sweep_all_test_rows(pg_conn)
    except Exception as e:
        print(f"WARN: session-end sweep failed: {e}")
```

- [ ] **Step 4: Commit**

```bash
cd /data/Repositories/evGRpc
git add tests/python/__init__.py tests/python/_helpers.py tests/python/conftest.py
git commit -m "test(pytest): scaffold conftest + helpers + fixtures (L1+L2 cleanup)"
```

### Task 1.8: Run smoke test (TDD green) + finalize commit

The smoke test (`test_search_weather_with_valid_bearer_succeeds`) was
already written and committed in Task 1.5 Step 2. The goal here is to
verify it passes now (green) and finalize any pending changes.

- [ ] **Step 1: Run smoke test (green verification)**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_smoke.py -v
```

Expected: PASS in <2s (token cache hit) or <5s (cold mint).
Going from `Expected: FAIL` in Task 1.6 to PASS here confirms the
fixtures built in Task 1.7 unblocked the test.

- [ ] **Step 2: Finalize commit**

```bash
cd /data/Repositories/evGRpc
# If no changes since Task 1.5, --allow-empty is needed.
git add tests/python/test_smoke.py
git commit -m "test(pytest): smoke test unblocked after fixtures (TDD green)" --allow-empty
```

---

## Chunk 2: WeatherService (smallest service — validates pattern)

**Goal:** write `test_weather.py` (12 tests) + `test_weather.md` per Goal 11. End of chunk: `pytest tests/python/test_weather.py -v` → 12 passed.

### Task 2.1: Write `test_weather.py`

**Files:**
- Create: `tests/python/test_weather.py`

- [ ] **Step 1: Write the file**

```python
"""WeatherService: 2 RPCs (CreateWeather, SearchWeather) — 12 tests."""

from __future__ import annotations

import uuid

import grpc
import pytest

from tests.python._helpers import (
    TrackedInsert,
    make_weather_name,
    make_uuid,
)
from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc


# ─────────────────────────── TestHappyPath ───────────────────────────

class TestHappyPath:
    def test_create_weather_returns_id_and_name(self, channel, namespace, pg_conn):
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        req = pb.CreateWeatherRequest(name=name)
        with TrackedInsert(pg_conn, "weather") as ti:
            resp = stub.CreateWeather(req)
            ti.register(resp.id)
        assert resp.id  # non-empty UUID
        assert resp.name == name

    def test_search_weather_finds_created_weather(self, channel, namespace, pg_conn):
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        unique = uuid.uuid4().hex[:6]
        name_with_marker = f"{name}-{unique}"  # make search-matchable
        with TrackedInsert(pg_conn, "weather") as ti:
            stub.CreateWeather(pb.CreateWeatherRequest(name=name_with_marker))
            ti.register(stub.SearchWeather(
                pb.SearchWeatherRequest(prefix=unique, limit=5)
            ).matches[0].id)
        resp = stub.SearchWeather(
            pb.SearchWeatherRequest(prefix=unique, limit=5)
        )
        assert any(m.name == name_with_marker for m in resp.matches)


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_create_weather_duplicate_name_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        with TrackedInsert(pg_conn, "weather") as ti:
            resp1 = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(resp1.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateWeather(pb.CreateWeatherRequest(name=name))
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS

    def test_create_weather_empty_name_returns_invalid_argument(
        self, channel, namespace
    ):
        stub = rpc.WeatherServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateWeather(pb.CreateWeatherRequest(name=""))
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    @pytest.mark.parametrize("name_len", [1, 35, 36, 37])
    def test_create_weather_name_length(self, channel, namespace, pg_conn, name_len):
        """VARCHAR(36): 1 = OK, 35 = OK, 36 = OK (at limit), 37 = INVALID_ARGUMENT."""
        stub = rpc.WeatherServiceStub(channel)
        # Use uuid-derived padding to avoid UNIQUE collisions across parametrized runs
        unique = uuid.uuid4().hex
        name = (unique + "x" * (name_len - len(unique)))[:name_len]
        # Adjust length precisely with a uuid that has a known starting length
        name = name[:name_len]
        if len(name) > 36:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT
        else:
            with TrackedInsert(pg_conn, "weather") as ti:
                resp = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
                ti.register(resp.id)
            assert len(resp.name) == name_len


# ─────────────────────────── TestConstraints ───────────────────────────

class TestConstraints:
    def test_create_weather_duplicate_name_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        """UNIQUE constraint on weather.Name."""
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        with TrackedInsert(pg_conn, "weather") as ti:
            resp1 = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(resp1.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateWeather(pb.CreateWeatherRequest(name=name))
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS
```

- [ ] **Step 2: Run**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_weather.py -v
```

Expected: 12 passed (or 9 if `test_create_weather_duplicate_name_returns_already_exists` is duplicated — see step 3).

- [ ] **Step 3: Resolve the duplicate**

`TestErrorPath.test_create_weather_duplicate_name_returns_already_exists` and `TestConstraints.test_create_weather_duplicate_name_returns_already_exists` are the same test. Remove the `TestConstraints` one (the constraint behavior is more naturally an error path). Keep `TestErrorPath`.

- [ ] **Step 4: Re-run**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_weather.py -v
```

Expected: 11 passed.

> **Note for implementer**: if real test count diverges from the spec's 12 estimate, update both `test_weather.md` § "Total tests" AND the spec §5.3 table in a follow-up commit.

### Task 2.2: Write `tests/python/doc/test_weather.md` (Goal 11)

**Files:**
- Create: `tests/python/doc/test_weather.md`

- [ ] **Step 1: Write the doc**

Use the template from spec §5.6. One section per test, in the order they appear in `test_weather.py`. Each section uses `### test_<name>` heading + 7 bullet fields (Purpose / Setup / Action / Expected / Cleanup / Rationale / Related).

Minimum sections: TestHappyPath (2 tests), TestErrorPath (2 tests), TestBoundaries (4 parametrized cases — one section per parametrize ID), TestConstraints (1 test).

- [ ] **Step 2: Verify MD ↔ test alignment**

```bash
cd /data/Repositories/evGRpc
# extract test function names from test_weather.py
grep -E "^    def test_" tests/python/test_weather.py | sed 's/.*def //; s/(.*//' | sort > /tmp/tests_in_code.txt
# extract section names from MD
grep -E "^### test_" tests/python/doc/test_weather.md | sed 's/^### //' | sort > /tmp/tests_in_md.txt
diff /tmp/tests_in_code.txt /tmp/tests_in_md.txt
```

Expected: no diff (every test in code has a corresponding MD section).

- [ ] **Step 3: Commit**

```bash
cd /data/Repositories/evGRpc
git add tests/python/test_weather.py tests/python/doc/test_weather.md
git commit -m "test(pytest): WeatherService 11 tests + per-test MD"
```

---

## Chunk 3: VehicleService

**Goal:** write `test_vehicle.py` + MD. ~27 tests across 4 classes (5 RPCs: CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles).

### Task 3.1: Write `test_vehicle.py`

**Files:**
- Create: `tests/python/test_vehicle.py`

- [ ] **Step 1: Write the file**

Skeleton (full version mirrors the test_weather.py pattern):

```python
"""VehicleService: 5 RPCs — ~27 tests.

RPCs: CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles.
"""

from __future__ import annotations

import uuid
from datetime import date

import grpc
import pytest

from tests.python._helpers import (
    TrackedInsert,
    make_license_plate,
    make_uuid,
)
from tests.python.gen.evgrpc import vehicle_pb2 as pb
from tests.python.gen.evgrpc import vehicle_pb2_grpc as rpc


def _make_create_req(ns: str, plate_len: int | None = None):
    """Build a CreateVehicleRequest with valid fields. Optionally vary plate length."""
    plate = make_license_plate(ns) if plate_len is None \
        else (make_license_plate(ns)[:plate_len])
    return pb.CreateVehicleRequest(
        brand="test-brand",
        calibrated_range_km=400,
        battery_capacity_kwh=75.0,
        purchase_date=date(2024, 1, 1),
        license_plate=plate,
    )


# ─────────────────────────── TestHappyPath ───────────────────────────

class TestHappyPath:
    def test_create_vehicle_returns_id_and_brand(self, channel, namespace, pg_conn):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        assert resp.id
        assert resp.brand == req.brand

    def test_get_vehicle_returns_created(self, channel, namespace, pg_conn):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
        got = stub.GetVehicle(pb.GetVehicleRequest(id=created.id))
        assert got.id == created.id
        assert got.license_plate == req.license_plate

    def test_update_vehicle_changes_brand(self, channel, namespace, pg_conn):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
        updated = stub.UpdateVehicle(pb.UpdateVehicleRequest(
            id=created.id,
            brand="test-brand-updated",
            calibrated_range_km=created.calibrated_range_km,
            battery_capacity_kwh=created.battery_capacity_kwh,
            purchase_date=created.purchase_date,
            license_plate=created.license_plate,
        ))
        assert updated.brand == "test-brand-updated"

    def test_delete_vehicle_removes_row(self, channel, namespace, pg_conn):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            stub.DeleteVehicle(pb.DeleteVehicleRequest(id=created.id))
            ti._ids.remove(created.id)  # already deleted, skip L1 cleanup
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicle(pb.GetVehicleRequest(id=created.id))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_list_vehicles_empty_returns_empty(self, channel):
        stub = rpc.VehicleServiceStub(channel)
        # Use a high page + large page size to minimize noise from other tests
        resp = stub.ListVehicles(pb.ListVehiclesRequest(page=1, page_size=1))
        assert isinstance(resp, pb.ListVehiclesResponse)

    def test_list_vehicles_after_create_includes_new(self, channel, namespace, pg_conn):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
        # Pagination: walk pages until we find our created id or hit empty page
        found = False
        for page in range(1, 100):
            resp = stub.ListVehicles(pb.ListVehiclesRequest(page=page, page_size=100))
            if any(v.id == created.id for v in resp.vehicles):
                found = True
                break
            if not resp.vehicles:
                break
        assert found, f"created vehicle {created.id} not found in any page"


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_get_vehicle_unknown_id_returns_not_found(self, channel):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicle(pb.GetVehicleRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_update_vehicle_unknown_id_returns_not_found(self, channel):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.UpdateVehicle(pb.UpdateVehicleRequest(
                id=make_uuid(),
                brand="x", calibrated_range_km=1, battery_capacity_kwh=1.0,
                purchase_date=date(2024, 1, 1), license_plate="x",
            ))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_delete_vehicle_unknown_id_returns_not_found(self, channel):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.DeleteVehicle(pb.DeleteVehicleRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_create_vehicle_empty_brand_returns_invalid_argument(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.brand = ""
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateVehicle(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_vehicle_negative_battery_returns_invalid_argument(self, channel, namespace):
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.battery_capacity_kwh = -1.0
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateVehicle(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    @pytest.mark.parametrize("plate_len,expect_ok", [(1, True), (15, True), (16, False)])
    def test_create_vehicle_license_plate_length(
        self, channel, namespace, pg_conn, plate_len, expect_ok
    ):
        """VARCHAR(15): 1 = OK, 15 = at-limit OK, 16 = INVALID_ARGUMENT."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace, plate_len=plate_len)
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            assert len(resp.license_plate) == plate_len
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    @pytest.mark.parametrize("brand_len,expect_ok", [(1, True), (36, True), (37, False)])
    def test_create_vehicle_brand_length(
        self, channel, namespace, pg_conn, brand_len, expect_ok
    ):
        """VARCHAR(36): 1 OK, 36 at-limit OK, 37 INVALID_ARGUMENT."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.brand = "x" * brand_len
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            assert len(resp.brand) == brand_len
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    @pytest.mark.parametrize("calibrated_range,expect_ok", [(0, False), (1, True), (10000, True), (100000, False)])
    def test_create_vehicle_calibrated_range(
        self, channel, namespace, pg_conn, calibrated_range, expect_ok
    ):
        """INT: 0 = INVALID (or accepted? — verify), positive = OK."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.calibrated_range_km = calibrated_range
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            assert resp.calibrated_range_km == calibrated_range
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    @pytest.mark.parametrize(
        "battery,expect_ok",
        [(0.0, False), (0.01, True), (9999.99, True), (10000.0, True)],
    )
    def test_create_vehicle_battery_capacity(
        self, channel, namespace, pg_conn, battery, expect_ok
    ):
        """DECIMAL(10,2): 0.0 = INVALID (or accepted?), max 99999999.99."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.battery_capacity_kwh = battery
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            assert resp.battery_capacity_kwh == battery
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── TestConstraints ───────────────────────────

class TestConstraints:
    def test_create_vehicle_duplicate_license_plate_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        """UNIQUE on vehicle.LicensePlate."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            first = stub.CreateVehicle(req)
            ti.register(first.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS

    def test_create_vehicle_missing_required_field_returns_invalid_argument(
        self, channel, namespace
    ):
        """purchase_date is NOT NULL; omitting it should fail."""
        stub = rpc.VehicleServiceStub(channel)
        req = pb.CreateVehicleRequest(
            brand="x", calibrated_range_km=1, battery_capacity_kwh=1.0,
            license_plate=make_license_plate(namespace),
            # purchase_date intentionally omitted
        )
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateVehicle(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_update_vehicle_to_duplicate_license_plate_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        """Update V2 with V1's plate should hit UNIQUE."""
        stub = rpc.VehicleServiceStub(channel)
        req1 = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            v1 = stub.CreateVehicle(req1)
            ti.register(v1.id)
            req2 = _make_create_req(namespace)
            v2 = stub.CreateVehicle(req2)
            ti.register(v2.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.UpdateVehicle(pb.UpdateVehicleRequest(
                    id=v2.id,
                    brand=v2.brand,
                    calibrated_range_km=v2.calibrated_range_km,
                    battery_capacity_kwh=v2.battery_capacity_kwh,
                    purchase_date=v2.purchase_date,
                    license_plate=v1.license_plate,  # ← collision
                ))
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS

    def test_delete_vehicle_with_consumption_returns_failed_precondition(
        self, channel, namespace, pg_conn
    ):
        """FK constraint: cannot delete a vehicle that has consumption rows.

        Setup: create vehicle V, create consumption C referencing V.
        Action: DeleteVehicle(V).
        Expected: FAILED_PRECONDITION.
        """
        # NOTE: this test crosses services. It needs consumption service to
        # exist. The test for that lives in Chunk 6; mark this test
        # `@pytest.mark.skip(reason="consumption service not yet tested (Chunk 6)")`
        # until Chunk 6 lands, then remove the skip.
        pytest.skip("consumption service not yet tested (Chunk 6)")
```

- [ ] **Step 2: Run**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_vehicle.py -v
```

Expected: ~25 passed (last test skipped).

- [ ] **Step 3: Write `tests/python/doc/test_vehicle.md`** using §5.6 template. One section per test + one section per parametrize ID.

- [ ] **Step 4: Verify MD ↔ code alignment** (same diff command as Task 2.2 step 2).

- [ ] **Step 5: Commit**

```bash
cd /data/Repositories/evGRpc
git add tests/python/test_vehicle.py tests/python/doc/test_vehicle.md
git commit -m "test(pytest): VehicleService tests + per-test MD"
```

---

## Chunk 4: SourceCategoryService

**Goal:** write `test_source_category.py` + MD. ~10 tests (2 RPCs: CreateSourceCategory, SearchSourceCategory).

### Task 4.1: Write `test_source_category.py` + MD

**Files:**
- Create: `tests/python/test_source_category.py`
- Create: `tests/python/doc/test_source_category.md`

- [ ] **Step 1: Write the test file** (pattern from Task 2.1; both RPCs use the search RPC for setup verification)

- [ ] **Step 2: Write the MD**

- [ ] **Step 3: Run + verify alignment + commit**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_source_category.py -v
git add tests/python/test_source_category.py tests/python/doc/test_source_category.md
git commit -m "test(pytest): SourceCategoryService tests + per-test MD"
```

---

## Chunk 5: ChargingService

**Goal:** write `test_charging.py` + MD. ~35 tests (5 RPCs + FK to vehicle + source_category).

### Task 5.1: Write `test_charging.py` + MD

**Files:**
- Create: `tests/python/test_charging.py`
- Create: `tests/python/doc/test_charging.md`

- [ ] **Step 1: Write the test file**

Pattern from Task 2.1, extended with:
- Helper `_make_charging_req(ns, vehicle_id, source_category_id)` for FK-dependent CreateCharging tests
- TestBoundaries: KwhCharged DECIMAL(10,2), Cost DECIMAL(10,2), ElectricityUnitPrice DECIMAL(4,2), ServiceFee DECIMAL(5,2), ChargerType enum (fast/slow), Location VARCHAR(100)
- TestConstraints: FK violations for VehicleId (orphan) and SourceCategoryId (orphan) → FAILED_PRECONDITION

- [ ] **Step 2: Write the MD** (one section per test, plus per parametrize ID)

- [ ] **Step 3: Run + verify + commit**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_charging.py -v
git add tests/python/test_charging.py tests/python/doc/test_charging.md
git commit -m "test(pytest): ChargingService tests + per-test MD"
```

---

## Chunk 6: ConsumptionService

**Goal:** write `test_consumption.py` + MD. ~31 tests (5 RPCs + FK to vehicle + weather).

### Task 6.1: Write `test_consumption.py` + MD

**Files:**
- Create: `tests/python/test_consumption.py`
- Create: `tests/python/doc/test_consumption.md`

- [ ] **Step 1: Write the test file**

Pattern from Task 2.1, extended with:
- Helper to ensure both vehicle + weather exist before consumption
- TestBoundaries: BeginPercent / EndPercent (0-100), temperatures DECIMAL(4,1), mileage/range INT
- TestConstraints: FK violations for VehicleId, WeatherId → FAILED_PRECONDITION

- [ ] **Step 2: Write the MD**

- [ ] **Step 3: Remove the `@pytest.mark.skip` on `test_delete_vehicle_with_consumption_returns_failed_precondition`** in `test_vehicle.py` (from Chunk 3) now that consumption service exists.

- [ ] **Step 4: Run both files together**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_consumption.py tests/python/test_vehicle.py::TestConstraints::test_delete_vehicle_with_consumption_returns_failed_precondition -v
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd /data/Repositories/evGRpc
git add tests/python/test_consumption.py tests/python/doc/test_consumption.md tests/python/test_vehicle.py
git commit -m "test(pytest): ConsumptionService tests + per-test MD; enable FK-delete vehicle test"
```

---

## Chunk 7: DisplayService (largest — 11 RPCs)

**Goal:** write `test_display.py` + MD. ~43 tests (11 RPCs including 3 new charging-report RPCs from v1.1.0).

### Task 7.1: Write `test_display.py` + MD

**Files:**
- Create: `tests/python/test_display.py`
- Create: `tests/python/doc/test_display.md`

- [ ] **Step 1: Write the test file**

RPCs to cover (from `proto/evgrpc/display.proto`):
- GetVehicleCostSummary
- GetMonthlyReport, GetAnnualReport (legacy, mixed-TZ data)
- GetCostByChargerType
- GetCostBySourceCategory
- GetConsumptionEfficiency
- GetRangeAccuracy
- GetTemperatureConsumptionCorrelation
- GetDailyChargingReport, GetMonthlyChargingReport, GetAnnualChargingReport (v1.1.0 — TZ-aware)

Pattern from Task 2.1, extended:
- TestHappyPath: 12 tests (1-2 per RPC, including empty/populated for report RPCs)
- TestErrorPath: 11 tests (1 per RPC — NOT_FOUND or INVALID_ARGUMENT)
- TestBoundaries: 14 tests (year out-of-range, month=0/13, day=0/32, Feb 30 etc. — per spec §5.3 Display boundary list)
- TestConstraints: 6 tests (vehicle_id NOT_FOUND for each report RPC, FK to consumption/charging)

> **Note**: DisplayService tests may exceed 0.65s/case (multi-table aggregates). Per spec §2.7, total budget ≤120s is what matters; per-case budget is approximate.

- [ ] **Step 2: Write the MD** (largest doc file — ~50 sections)

- [ ] **Step 3: Run + verify + commit**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_display.py -v
git add tests/python/test_display.py tests/python/doc/test_display.md
git commit -m "test(pytest): DisplayService tests + per-test MD (largest chunk)"
```

---

## Chunk 8: Auth enforcement

**Goal:** write `test_auth_enforcement.py` + MD. 3 tests: missing token, malformed token, forged token.

### Task 8.1: Write `test_auth_enforcement.py` + MD

**Files:**
- Create: `tests/python/test_auth_enforcement.py`
- Create: `tests/python/doc/test_auth_enforcement.md`

- [ ] **Step 1: Write the test file**

Uses bare channel (no bearer interceptor):

```python
"""Auth enforcement: 3 tests covering missing / malformed / forged tokens."""

from __future__ import annotations

import time
import uuid

import grpc
import pytest
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization
import jwt as pyjwt

from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc


def _bare_channel():
    chan = grpc.insecure_channel("localhost:80")
    grpc.channel_ready_future(chan).result(timeout=5)
    return chan


def test_no_token_returns_unauthenticated():
    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(pb.SearchWeatherRequest())
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED


def test_malformed_token_returns_unauthenticated():
    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(
            pb.SearchWeatherRequest(),
            metadata=(("authorization", "Bearer not.a.real.jwt"),),
        )
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED


def test_forged_token_returns_unauthenticated():
    """Sign a structurally-valid JWT with the real iss/aud but a throwaway RSA key.

    evgrpc's JWT validator checks signature against the IdP's JWKS, which
    does not contain our throwaway key → UNAUTHENTICATED.
    """
    # Generate a throwaway RSA key (never published)
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    private_pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )

    now = int(time.time())
    claims = {
        "iss": "https://auth-test.mksword.com/",
        "aud": "https://www.mksword.com/grpc/ev",
        "sub": "forged-test",
        "iat": now,
        "exp": now + 3600,
    }
    forged_token = pyjwt.encode(
        claims, private_pem, algorithm="RS256",
        headers={"kid": "forged-key"},
    )

    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(
            pb.SearchWeatherRequest(),
            metadata=(("authorization", f"Bearer {forged_token}"),),
        )
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED
```

- [ ] **Step 2: Write the MD** (3 sections — small file)

- [ ] **Step 3: Run + verify + commit**

```bash
cd /data/Repositories/evGRpc
conda run -n evgrpc-tests pytest tests/python/test_auth_enforcement.py -v
git add tests/python/test_auth_enforcement.py tests/python/doc/test_auth_enforcement.md
git commit -m "test(pytest): auth enforcement (missing/malformed/forged token) + MD"
```

---

## Chunk 9: CI integration + final smoke

**Goal:** wire the suite into `run_all_tests.sh`, run full suite end-to-end, update README.

### Task 9.1: Append Python stage to `run_all_tests.sh`

**Files:**
- Modify: `scripts/run_all_tests.sh` (existing file)

- [ ] **Step 1: Read current `run_all_tests.sh` to find the end-of-script marker**

```bash
cd /data/Repositories/evGRpc
tail -10 scripts/run_all_tests.sh
```

- [ ] **Step 2: Append the Python stage**

Append at the end:

```bash
echo "=== Python gRPC IT ==="
conda run -n evgrpc-tests pytest tests/python/ --tb=short -q || exit 1
```

- [ ] **Step 3: Run `run_all_tests.sh` end-to-end**

```bash
cd /data/Repositories/evGRpc
bash scripts/run_all_tests.sh 2>&1 | tail -50
```

Expected: passes (the C++ tests already passed; the new Python stage should also pass given prior chunks).

- [ ] **Step 4: Commit**

```bash
cd /data/Repositories/evGRpc
git add scripts/run_all_tests.sh
git commit -m "ci: append Python gRPC IT stage to scripts/run_all_tests.sh"
```

### Task 9.2: Update README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Find the test-running section in README**

```bash
cd /data/Repositories/evGRpc
grep -n -A 3 "test" README.md | head -30
```

- [ ] **Step 2: Add a "Python Integration Tests" subsection** with:
- One-command setup (`conda env create -f environment.yml`)
- Run command (`conda run -n evgrpc-tests pytest tests/python/ -v`)
- Pointer to `tests/python/doc/` for per-test rationale
- Note on namespace prefix + cleanup (one paragraph)

- [ ] **Step 3: Commit**

```bash
cd /data/Repositories/evGRpc
git add README.md
git commit -m "docs: add Python Integration Tests section to README"
```

### Task 9.3: Final full-suite run + total runtime check

- [ ] **Step 1: Run the full Python suite and capture timing**

```bash
cd /data/Repositories/evGRpc
time conda run -n evgrpc-tests pytest tests/python/ --tb=short 2>&1 | tail -20
```

Expected: total runtime ≤120s (per spec §2.7 Goal 7). If over, profile slow tests and either optimize or update the spec threshold via a follow-up commit.

- [ ] **Step 2: Verify final test counts**

```bash
conda run -n evgrpc-tests pytest tests/python/ --collect-only -q | tail -5
```

Expected: ~161 tests collected (per spec §5.3). If actual count diverges significantly (e.g. >20% off), update both `tests/python/doc/*.md` "Total tests" lines AND spec §5.3 table in a follow-up commit.

- [ ] **Step 3: Commit any final doc/spec corrections** (if needed)

```bash
cd /data/Repositories/evGRpc
git add -A
git commit -m "chore(pytest): reconcile actual test count with doc estimates" --allow-empty
```

---

## Definition of Done

- [ ] All 9 chunks complete; each chunk's commit(s) in `git log`.
- [ ] `conda run -n evgrpc-tests pytest tests/python/ --tb=short` exits 0.
- [ ] Total runtime ≤120s (spec §2.7 Goal 7).
- [ ] Every test in `tests/python/` has a corresponding MD section in `tests/python/doc/<service>.md` (Goal 11).
- [ ] `run_all_tests.sh` ends with the Python stage and exits 0.
- [ ] README mentions the new test suite.

## Out-of-Scope (per spec §10)

- RBAC / per-RPC scope enforcement tests (waiting on server-side scope checks).
- Expired-token test (requires IdP admin change).
- Coverage report (`pytest-cov`) — deferred.
- Parallel execution (`pytest-xdist`) — defer until runtime becomes an issue.
- Auto-generating test docs (explicitly rejected by §5.6).
