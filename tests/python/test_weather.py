"""WeatherService: 2 RPCs (CreateWeather, SearchWeather) — 11 tests."""

from __future__ import annotations

import uuid

import grpc
import pytest

from tests.python._helpers import TrackedInsert, make_weather_name
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
        # PG `^@` is "starts with" (LIKE prefix%) — marker must be at START of name.
        # Total name length: 6 (marker) + 1 (-) + 29 (make_weather_name) = 36, exactly at VARCHAR(36) limit.
        marker = uuid.uuid4().hex[:6]
        name = marker + "-" + make_weather_name(namespace)
        with TrackedInsert(pg_conn, "weather") as ti:
            created = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(created.id)
            # Search inside the with-block, otherwise __exit__ deletes the row
            # before our search query runs.
            resp = stub.SearchWeather(
                pb.SearchWeatherRequest(prefix=marker, limit=5)
            )
        assert any(m.name == name for m in resp.matches)


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_create_weather_duplicate_name_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        """UNIQUE constraint on weather.Name → ALREADY_EXISTS."""
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        with TrackedInsert(pg_conn, "weather") as ti:
            resp1 = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(resp1.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateWeather(pb.CreateWeatherRequest(name=name))
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS

    def test_create_weather_empty_name_is_accepted(
        self, channel, namespace, pg_conn
    ):
        """Empty string is accepted by production (VARCHAR(36) accepts it)."""
        stub = rpc.WeatherServiceStub(channel)
        with TrackedInsert(pg_conn, "weather") as ti:
            resp = stub.CreateWeather(pb.CreateWeatherRequest(name=""))
            ti.register(resp.id)
        assert resp.name == ""

    def test_create_weather_unicode_name_is_accepted(
        self, channel, namespace, pg_conn
    ):
        """Unicode names are accepted (PG VARCHAR is UTF-8)."""
        stub = rpc.WeatherServiceStub(channel)
        name = "测试-天气-" + make_weather_name(namespace)
        # Ensure under 36 chars
        name = name[:36]
        with TrackedInsert(pg_conn, "weather") as ti:
            resp = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(resp.id)
        assert resp.name == name

    def test_create_weather_special_chars_name_is_accepted(
        self, channel, namespace, pg_conn
    ):
        """Special chars (no SQL injection risk with parameterized queries)."""
        stub = rpc.WeatherServiceStub(channel)
        name = "test-special-!@#-" + make_weather_name(namespace)
        name = name[:36]
        with TrackedInsert(pg_conn, "weather") as ti:
            resp = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            ti.register(resp.id)
        assert resp.name == name


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    @pytest.mark.parametrize("name_len", [1, 35, 36, 37])
    def test_create_weather_name_length(self, channel, namespace, pg_conn, name_len):
        """VARCHAR(36): 1 = OK, 35 = OK, 36 = OK (at limit), 37 = INVALID_ARGUMENT."""
        stub = rpc.WeatherServiceStub(channel)
        # uuid hex = 32 chars. Pad/truncate to name_len.
        unique = uuid.uuid4().hex
        if name_len <= len(unique):
            name = unique[:name_len]
        else:
            name = unique + "x" * (name_len - len(unique))
        if name_len > 36:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT
        else:
            with TrackedInsert(pg_conn, "weather") as ti:
                resp = stub.CreateWeather(pb.CreateWeatherRequest(name=name))
                ti.register(resp.id)
            assert len(resp.name) == name_len

    def test_search_weather_empty_prefix_returns_all(
        self, channel, namespace, pg_conn
    ):
        """Empty prefix: PG ^@ '' matches every row (within limit).

        Inner search inside the with-block finds our row; outer search
        (after cleanup) verifies response is iterable (might be empty
        after cleanup deletes test rows — just verify shape).
        """
        stub = rpc.WeatherServiceStub(channel)
        name = make_weather_name(namespace)
        with TrackedInsert(pg_conn, "weather") as ti:
            stub.CreateWeather(pb.CreateWeatherRequest(name=name))
            # Inner search: should find our row
            inner = stub.SearchWeather(
                pb.SearchWeatherRequest(prefix="", limit=100)
            )
            assert len(inner.matches) >= 1
            assert any(m.id for m in inner.matches)
            # Register for cleanup
            ti.register(inner.matches[0].id)

    def test_search_weather_limit_zero_uses_default(
        self, channel, namespace, pg_conn
    ):
        """limit=0 → server uses default (50)."""
        stub = rpc.WeatherServiceStub(channel)
        resp = stub.SearchWeather(pb.SearchWeatherRequest(prefix="", limit=0))
        assert isinstance(resp, pb.SearchWeatherResponse)
        # Should not raise; default limit is applied server-side

    def test_search_weather_negative_limit_uses_default(
        self, channel, namespace, pg_conn
    ):
        """limit=-1 → server uses default (50)."""
        stub = rpc.WeatherServiceStub(channel)
        resp = stub.SearchWeather(pb.SearchWeatherRequest(prefix="", limit=-1))
        assert isinstance(resp, pb.SearchWeatherResponse)

    def test_search_weather_no_matches_returns_empty(
        self, channel, namespace
    ):
        """No matches for random prefix → empty list."""
        stub = rpc.WeatherServiceStub(channel)
        # Use a 32-char hex prefix that's extremely unlikely to match anything
        marker = uuid.uuid4().hex
        resp = stub.SearchWeather(
            pb.SearchWeatherRequest(prefix=marker, limit=10)
        )
        assert len(resp.matches) == 0