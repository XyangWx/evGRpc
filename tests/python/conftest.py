"""Session-scoped pytest fixtures for the evGRpc test suite."""

from __future__ import annotations

# Force UTC for the entire test session BEFORE any other import. Without
# this, naive `datetime(...)` values get encoded as local time when sent
# through protobuf's `Timestamp.FromDatetime` (which treats naive
# datetime as **local**), and any code path that calls `datetime.now()`
# / `time.localtime()` inherits the host TZ. Tests that already use
# `tzinfo=timezone.utc` are unaffected by this; tests that still pass
# naive `datetime(...)` will be caught by an explicit assertion in the
# module-level `_helpers` guard or by the codemod that rewrote them.
import os
os.environ['TZ'] = 'UTC'
try:
    import time as _time
    _time.tzset()
except (AttributeError, OSError):
    # tzset() may be unavailable on some platforms (notably Windows).
    # Project targets are Linux/macOS, where this always succeeds.
    pass

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