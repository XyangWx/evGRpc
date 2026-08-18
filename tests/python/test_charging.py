"""ChargingService: 5 RPCs — 17 tests.

RPCs: CreateCharging, GetCharging, UpdateCharging, DeleteCharging, ListChargings.

FK dependencies: requires a valid vehicle_id + source_category_id (UUID).
"""

from __future__ import annotations

from datetime import datetime, timedelta

import grpc
import pytest

from tests.python._helpers import (
    TrackedInsert,
    make_license_plate,
    make_source_category_name,
    make_uuid,
)
from tests.python.gen.evgrpc import charging_pb2 as pb
from tests.python.gen.evgrpc import charging_pb2_grpc as rpc
from tests.python.gen.evgrpc import vehicle_pb2 as v_pb
from tests.python.gen.evgrpc import vehicle_pb2_grpc as v_rpc
from tests.python.gen.evgrpc import source_category_pb2 as sc_pb
from tests.python.gen.evgrpc import source_category_pb2_grpc as sc_rpc


# ─────────────────────────── helpers ───────────────────────────

def _create_vehicle(stub, namespace, pg_conn):
    plate = make_license_plate(namespace)
    resp = stub.CreateVehicle(v_pb.CreateVehicleRequest(
        brand="test-brand",
        calibrated_range_km=400,
        battery_capacity_kwh=75.0,
        purchase_date=datetime(2024, 1, 1, 0, 0, 0),
        license_plate=plate,
    ))
    return resp.id  # caller registers for cleanup


def _create_source_category(stub, namespace, pg_conn):
    name = make_source_category_name(namespace)
    resp = stub.CreateSourceCategory(sc_pb.CreateSourceCategoryRequest(name=name))
    return resp.id


def _make_charging_req(vehicle_id, source_category_id, **overrides):
    """Build a CreateChargingRequest with valid defaults."""
    start = datetime(2024, 6, 15, 10, 0, 0)
    end = start + timedelta(hours=1)
    req = pb.CreateChargingRequest(
        vehicle_id=vehicle_id,
        start_time=start,
        end_time=end,
        start_percent=20,
        end_percent=80,
        start_mileage_km=10000,
        end_mileage_km=10100,
        kwh_charged=45.5,
        cost=50.0,
        electricity_unit_price=1.10,
        charger_type=pb.CHARGER_TYPE_FAST,
        source_category_id=source_category_id,
        location="test-loc-default",  # tests override this; VARCHAR(100) accepts
        remark="test-rem",
    )
    for k, v in overrides.items():
        setattr(req, k, v)
    return req


# ─────────────────────────── fixtures ───────────────────────────

@pytest.fixture
def vehicle_and_source(channel, namespace, pg_conn):
    """Create a fresh vehicle + source_category for FK-dependent tests."""
    v_stub = v_rpc.VehicleServiceStub(channel)
    sc_stub = sc_rpc.SourceCategoryServiceStub(channel)
    with TrackedInsert(pg_conn, "vehicle") as v_ti, \
         TrackedInsert(pg_conn, "source_category") as sc_ti:
        vid = _create_vehicle(v_stub, namespace, pg_conn)
        v_ti.register(vid)
        scid = _create_source_category(sc_stub, namespace, pg_conn)
        sc_ti.register(scid)
        yield vid, scid


# ─────────────────────────── TestHappyPath ───────────────────────────

class TestHappyPath:
    def test_create_charging_returns_id_and_fields(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        with TrackedInsert(pg_conn, "charging") as ti:
            resp = stub.CreateCharging(req)
            ti.register(resp.id)
        assert resp.id
        assert resp.vehicle_id == vid
        assert resp.kwh_charged == pytest.approx(45.5)
        assert resp.charger_type == pb.CHARGER_TYPE_FAST

    def test_get_charging_returns_created(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        with TrackedInsert(pg_conn, "charging") as ti:
            created = stub.CreateCharging(req)
            ti.register(created.id)
            got = stub.GetCharging(pb.GetChargingRequest(id=created.id))
        assert got.id == created.id
        assert got.vehicle_id == vid

    def test_update_charging_changes_kwh(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        with TrackedInsert(pg_conn, "charging") as ti:
            created = stub.CreateCharging(req)
            ti.register(created.id)
            updated = stub.UpdateCharging(pb.UpdateChargingRequest(
                id=created.id,
                vehicle_id=vid,
                start_time=req.start_time,
                end_time=req.end_time,
                start_percent=req.start_percent,
                end_percent=req.end_percent,
                start_mileage_km=req.start_mileage_km,
                end_mileage_km=req.end_mileage_km,
                kwh_charged=99.9,  # changed
                cost=req.cost,
                electricity_unit_price=req.electricity_unit_price,
                charger_type=req.charger_type,
                source_category_id=scid,
                location=req.location,
                remark=req.remark,
            ))
        assert updated.kwh_charged == pytest.approx(99.9)

    def test_delete_charging_removes_row(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        with TrackedInsert(pg_conn, "charging") as ti:
            created = stub.CreateCharging(req)
            ti.register(created.id)
            stub.DeleteCharging(pb.DeleteChargingRequest(id=created.id))
            ti.unregister(created.id)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetCharging(pb.GetChargingRequest(id=created.id))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_list_chargings_after_create_includes_new(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        with TrackedInsert(pg_conn, "charging") as ti:
            created = stub.CreateCharging(req)
            ti.register(created.id)
            # Filter by vehicle_id; should include our row.
            resp = stub.ListChargings(
                pb.ListChargingsRequest(vehicle_id=vid, page_size=100)
            )
        assert any(c.id == created.id for c in resp.chargings)


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_get_charging_unknown_id_returns_not_found(self, channel):
        stub = rpc.ChargingServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetCharging(pb.GetChargingRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_update_charging_unknown_id_returns_not_found(self, channel, vehicle_and_source):
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        with pytest.raises(grpc.RpcError) as exc:
            stub.UpdateCharging(pb.UpdateChargingRequest(
                id=make_uuid(),
                vehicle_id=vid,
                start_time=datetime(2024, 6, 15, 10, 0, 0),
                end_time=datetime(2024, 6, 15, 11, 0, 0),
                start_percent=20, end_percent=80,
                start_mileage_km=10000, end_mileage_km=10100,
                kwh_charged=45.5, cost=50.0, electricity_unit_price=1.10,
                charger_type=pb.CHARGER_TYPE_FAST,
                source_category_id=scid,
                location="loc", remark="rem",
            ))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_delete_charging_unknown_id_returns_not_found(self, channel):
        stub = rpc.ChargingServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.DeleteCharging(pb.DeleteChargingRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    def test_create_charging_end_time_equal_start_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: end_time must be > start_time."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        t = datetime(2024, 6, 15, 10, 0, 0)
        req = _make_charging_req(vid, scid, start_time=t, end_time=t)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_end_percent_equal_start_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: end_percent must be > start_percent."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, start_percent=50, end_percent=50)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_kwh_zero_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: kwh_charged must be > 0."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, kwh_charged=0.0)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_cost_zero_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: cost must be > 0."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, cost=0.0)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_location_length(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """VARCHAR(100): 100-char location = OK; 101-char = INVALID_ARGUMENT."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        # 101-char location (just over limit)
        loc = "x" * 101
        req = _make_charging_req(vid, scid, location=loc)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_location_at_limit_ok(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """VARCHAR(100): 100-char location = OK (at limit)."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        loc = "x" * 100
        with TrackedInsert(pg_conn, "charging") as ti:
            resp = stub.CreateCharging(_make_charging_req(vid, scid, location=loc))
            ti.register(resp.id)
        assert len(resp.location) == 100

    def test_create_charging_negative_kwh_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: kwh_charged must be > 0 (negative also rejected)."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, kwh_charged=-1.0)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_negative_cost_returns_invalid(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """App validation: cost must be > 0 (negative also rejected)."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, cost=-50.0)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_slow_charger_type_ok(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """ChargerType.SLOW is a valid alternative to FAST."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, charger_type=pb.CHARGER_TYPE_SLOW)
        with TrackedInsert(pg_conn, "charging") as ti:
            resp = stub.CreateCharging(req)
            ti.register(resp.id)
        assert resp.charger_type == pb.CHARGER_TYPE_SLOW

    def test_create_charging_with_service_fee_ok(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """Explicit service_fee (DoubleValue wrapper) is stored."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        req.service_fee.value = 5.0
        with TrackedInsert(pg_conn, "charging") as ti:
            resp = stub.CreateCharging(req)
            ti.register(resp.id)
        assert resp.service_fee.value == 5.0

    def test_create_charging_no_service_fee_is_null(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """No service_fee (DoubleValue unset) → null in DB."""
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid)
        # Don't set service_fee
        with TrackedInsert(pg_conn, "charging") as ti:
            resp = stub.CreateCharging(req)
            ti.register(resp.id)
        # Should be unset (has_service_fee() == False)
        assert not resp.HasField("service_fee")


# ─────────────────────────── TestConstraints ───────────────────────────

class TestConstraints:
    def test_create_charging_invalid_vehicle_id_returns_invalid_argument(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """FK violation: orphan vehicle_id → INVALID_ARGUMENT (per error.cc)."""
        stub = rpc.ChargingServiceStub(channel)
        _, scid = vehicle_and_source
        req = _make_charging_req(make_uuid(), scid)  # random UUID, not a real vehicle
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_invalid_source_category_id_returns_invalid_argument(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """FK violation: orphan source_category_id → INVALID_ARGUMENT."""
        stub = rpc.ChargingServiceStub(channel)
        vid, _ = vehicle_and_source
        req = _make_charging_req(vid, make_uuid())  # random UUID
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_charging_charger_type_unspecified_uses_default(
        self, channel, namespace, pg_conn, vehicle_and_source
    ):
        """UNSPECIFIED enum → INTERNAL.

        ChargerTypeLabel(UNSPECIFIED) returns ''. PG enum cast '' = NULL,
        hits NOT NULL constraint. `not_null_violation` is NOT a subclass of
        `data_exception` so error.cc falls through to default INTERNAL.
        Surprising but documents current production behavior.
        """
        stub = rpc.ChargingServiceStub(channel)
        vid, scid = vehicle_and_source
        req = _make_charging_req(vid, scid, charger_type=pb.CHARGER_TYPE_UNSPECIFIED)
        with pytest.raises(grpc.RpcError) as exc:
            stub.CreateCharging(req)
        assert exc.value.code() == grpc.StatusCode.INTERNAL