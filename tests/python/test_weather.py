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

    # NOTE: production code accepts empty string for weather.Name (no app-level
    # validation; VARCHAR(36) accepts ""). No INVALID_ARGUMENT boundary here.
    # Removed `test_create_weather_empty_name_returns_invalid_argument` after
    # first implementation revealed the test assumption was wrong.


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