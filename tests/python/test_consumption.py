"""ConsumptionService: 5 RPCs — ~15 tests.

RPCs: CreateConsumption, GetConsumption, UpdateConsumption, DeleteConsumption, ListConsumptions.

FK deps: vehicle_id (required), weather_id (optional/nullable).
"""

from __future__ import annotations

import uuid
from datetime import datetime, timedelta

import grpc
import pytest

from tests.python._helpers import (
    TrackedInsert,
    make_license_plate,
    make_uuid,
    make_weather_name,
)
from tests.python.gen.evgrpc import consumption_pb2 as pb
from tests.python.gen.evgrpc import consumption_pb2_grpc as rpc
from tests.python.gen.evgrpc import vehicle_pb2 as v_pb
from tests.python.gen.evgrpc import vehicle_pb2_grpc as v_rpc
from tests.python.gen.evgrpc import weather_pb2 as w_pb
from tests.python.gen.evgrpc import weather_pb2_grpc as w_rpc


# ─────────────────────────── helpers ───────────────────────────

def _create_vehicle(stub, namespace):
    plate = make_license_plate(namespace)
    return stub.CreateVehicle(v_pb.CreateVehicleRequest(
        brand="test-brand",
        calibrated_range_km=400,
        battery_capacity_kwh=75.0,
        purchase_date=datetime(2024, 1, 1, 0, 0, 0),
        license_plate=plate,
    )).id


def _create_weather(stub, namespace):
    name = make_weather_name(namespace)
    return stub.CreateWeather(w_pb.CreateWeatherRequest(name=name)).id


def _make_consumption_req(vehicle_id, weather_id=None, **overrides):
    start = datetime(2024, 6, 15, 10, 0, 0)
    end = start + timedelta(hours=2)
    req = pb.CreateConsumptionRequest(
        vehicle_id=vehicle_id,
        start=start,
        end=end,
        begin_percent=80,
        end_percent=40,  # drains
        begin_mileage_km=10100,
        end_mileage_km=10200,
        begin_range_km=320,
        end_range_km=160,
        highest_temperature_c=25.0,
        lowest_temperature_c=15.0,
        weather_id=weather_id or "",
        remark="test-rem",
    )
    for k, v in overrides.items():
        setattr(req, k, v)
    return req


@pytest.fixture
def vehicle_and_weather(channel, namespace, pg_conn):
    """Create a fresh vehicle + weather for FK-dependent tests."""
    v_stub = v_rpc.VehicleServiceStub(channel)
    w_stub = w_rpc.WeatherServiceStub(channel)
    with TrackedInsert(pg_conn, "vehicle") as v_ti, \
         TrackedInsert(pg_conn, "weather") as w_ti:
        vid = _create_vehicle(v_stub, namespace)
        v_ti.register(vid)
        wid = _create_weather(w_stub, namespace)
        w_ti.register(wid)
        yield vid, wid


# ─────────────────────────── TestHappyPath ───────────────────────────

class TestHappyPath:
    def test_create_consumption_returns_id_and_fields(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid)
        with TrackedInsert(pg_conn, "consumption") as ti:
            resp = stub.CreateConsumption(req)
            ti.register(resp.id)
        assert resp.id
        assert resp.vehicle_id == vid
        assert resp.weather_id == wid
        assert resp.begin_percent == 80
        assert resp.end_percent == 40

    def test_get_consumption_returns_created(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid)
        with TrackedInsert(pg_conn, "consumption") as ti:
            created = stub.CreateConsumption(req)
            ti.register(created.id)
            got = stub.GetConsumption(pb.GetConsumptionRequest(id=created.id))
        assert got.id == created.id

    def test_update_consumption_changes_end_percent(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid)
        with TrackedInsert(pg_conn, "consumption") as ti:
            created = stub.CreateConsumption(req)
            ti.register(created.id)
            updated = stub.UpdateConsumption(pb.UpdateConsumptionRequest(
                id=created.id,
                vehicle_id=vid,
                start=req.start,
                end=req.end,
                begin_percent=req.begin_percent,
                end_percent=30,  # changed (was 40)
                begin_mileage_km=req.begin_mileage_km,
                end_mileage_km=req.end_mileage_km,
                begin_range_km=req.begin_range_km,
                end_range_km=req.end_range_km,
                highest_temperature_c=req.highest_temperature_c,
                lowest_temperature_c=req.lowest_temperature_c,
                weather_id=wid,
                remark=req.remark,
            ))
        assert updated.end_percent == 30

    def test_delete_consumption_removes_row(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid)
        with TrackedInsert(pg_conn, "consumption") as ti:
            created = stub.CreateConsumption(req)
            ti.register(created.id)
            stub.DeleteConsumption(pb.DeleteConsumptionRequest(id=created.id))
            ti.unregister(created.id)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetConsumption(pb.GetConsumptionRequest(id=created.id))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_list_consumptions_after_create_includes_new(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid)
        with TrackedInsert(pg_conn, "consumption") as ti:
            created = stub.CreateConsumption(req)
            ti.register(created.id)
            resp = stub.ListConsumptions(
                pb.ListConsumptionsRequest(vehicle_id=vid, page_size=100)
            )
        assert any(c.id == created.id for c in resp.consumptions)


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_get_consumption_unknown_id_returns_not_found(self, channel):
        stub = rpc.ConsumptionServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetConsumption(pb.GetConsumptionRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_update_consumption_unknown_id_returns_not_found(
        self, channel, vehicle_and_weather
    ):
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        with pytest.raises(grpc.RpcError) as exc:
            stub.UpdateConsumption(pb.UpdateConsumptionRequest(
                id=make_uuid(),
                vehicle_id=vid,
                start=datetime(2024, 6, 15, 10, 0, 0),
                end=datetime(2024, 6, 15, 12, 0, 0),
                begin_percent=80, end_percent=40,
                begin_mileage_km=100, end_mileage_km=200,
                begin_range_km=320, end_range_km=160,
                highest_temperature_c=25.0, lowest_temperature_c=15.0,
                weather_id=wid, remark="x",
            ))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_delete_consumption_unknown_id_returns_not_found(self, channel):
        stub = rpc.ConsumptionServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.DeleteConsumption(pb.DeleteConsumptionRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    def test_create_consumption_end_equal_start_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """App validation: end > start."""
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        t = datetime(2024, 6, 15, 10, 0, 0)
        req = _make_consumption_req(vid, wid, start=t, end=t)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_consumption_end_percent_geq_begin_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """App validation: end_percent < begin_percent (consumption drains)."""
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(vid, wid, begin_percent=40, end_percent=80)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_consumption_highest_lt_lowest_temp_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """App validation: highest_temperature >= lowest_temperature."""
        stub = rpc.ConsumptionServiceStub(channel)
        vid, wid = vehicle_and_weather
        req = _make_consumption_req(
            vid, wid, highest_temperature_c=10.0, lowest_temperature_c=20.0
        )
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── TestConstraints ───────────────────────────

class TestConstraints:
    def test_create_consumption_invalid_vehicle_id_returns_invalid_argument(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """FK violation: orphan vehicle_id → INVALID_ARGUMENT (per error.cc)."""
        stub = rpc.ConsumptionServiceStub(channel)
        _, wid = vehicle_and_weather
        req = _make_consumption_req(make_uuid(), wid)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_consumption_invalid_weather_id_returns_invalid_argument(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """FK violation: orphan weather_id → INVALID_ARGUMENT."""
        stub = rpc.ConsumptionServiceStub(channel)
        vid, _ = vehicle_and_weather
        req = _make_consumption_req(vid, make_uuid())
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_consumption_empty_weather_id_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_weather
    ):
        """Empty weather_id → INVALID_ARGUMENT (UUID syntax error).

        Documented production behavior: WeatherId column is nullable in DB
        (sql/001_initial.sql:39), but production code binds $13 as raw
        string, so empty string fails UUID cast. The SQL comment claims
        NULLIF('') wrap, but the actual SQL is just `$13` (production
        bug — but out of scope to fix here). Documenting the current
        behavior so a future fix can update this test.
        """
        stub = rpc.ConsumptionServiceStub(channel)
        vid, _ = vehicle_and_weather
        req = _make_consumption_req(vid, "")  # empty weather_id
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateConsumption(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT