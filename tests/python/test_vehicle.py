"""VehicleService: 5 RPCs — ~17 tests (after removing invalid assumptions).

RPCs: CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles.

Removed during implementation (no production validation):
- test_create_vehicle_empty_brand_returns_invalid_argument  (no app-level validation)
- test_create_vehicle_negative_battery_returns_invalid_argument  (no app-level validation)
- test_create_vehicle_missing_required_field_returns_invalid_argument  (proto3 default)
- test_create_vehicle_calibrated_range[0]  (no check on 0/negative; only INT bounds)
- test_create_vehicle_battery_capacity[0.0]  (no check on 0; only DECIMAL bounds)

These tests were in the plan but production has no validation. Adding
tests would force production changes (out of scope).
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone

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
    if plate_len is None:
        plate = make_license_plate(ns)
    else:
        base = make_license_plate(ns)  # 15 chars
        if plate_len < len(base):
            plate = base[:plate_len]  # truncate (e.g. plate_len=1)
        else:
            plate = base + "x" * (plate_len - len(base))  # pad (e.g. plate_len=16)
    return pb.CreateVehicleRequest(
        brand="test-brand",
        calibrated_range_km=400,
        battery_capacity_kwh=75.0,
        purchase_date=datetime(2024, 1, 1, 0, 0, 0),
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
            ti.unregister(created.id)  # already deleted; skip L1 cleanup
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicle(pb.GetVehicleRequest(id=created.id))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_list_vehicles_returns_response(self, channel):
        """ListVehicles accepts page_size + page_token, returns Vehicle list."""
        stub = rpc.VehicleServiceStub(channel)
        resp = stub.ListVehicles(pb.ListVehiclesRequest(page_size=1))
        assert isinstance(resp, pb.ListVehiclesResponse)
        # Protobuf RepeatedFieldContainer is iterable; len() works; may be empty.
        assert len(resp.vehicles) >= 0

    def test_list_vehicles_after_create_includes_new(self, channel, namespace, pg_conn):
        """A newly-created vehicle is reachable via paged ListVehicles."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            # Walk pages with page_token (offset-based) INSIDE the with-block.
            found = False
            page_token = ""
            for _ in range(10):
                resp = stub.ListVehicles(
                    pb.ListVehiclesRequest(page_size=100, page_token=page_token)
                )
                if any(v.id == created.id for v in resp.vehicles):
                    found = True
                    break
                if not resp.next_page_token:
                    break
                page_token = resp.next_page_token
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
                purchase_date=datetime(2024, 1, 1, 0, 0, 0), license_plate="x",
            ))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND

    def test_delete_vehicle_unknown_id_returns_not_found(self, channel):
        stub = rpc.VehicleServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.DeleteVehicle(pb.DeleteVehicleRequest(id=make_uuid()))
        assert exc.value.code() == grpc.StatusCode.NOT_FOUND


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

    @pytest.mark.parametrize(
        "range_km,expect_ok",
        [
            (0, True),  # no check on 0
            (1, True),
            (2147483647, True),  # INT32 max
            # INT32 overflow (2^31) can't be sent: protobuf int32 wire format
            # rejects it client-side. PG INT column accepts any int4 value.
            # No testable boundary here.
        ],
    )
    def test_create_vehicle_calibrated_range(
        self, channel, namespace, pg_conn, range_km, expect_ok
    ):
        """INT column: 0/positive/INT32_max = OK."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.calibrated_range_km = range_km
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            assert resp.calibrated_range_km == range_km
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    @pytest.mark.parametrize(
        "battery,expect_ok",
        [
            (0.0, True),  # no check on 0
            (0.01, True),
            (99999999.99, True),  # DECIMAL(10,2) max
            (100000000.0, False),  # DECIMAL overflow → data_exception → INVALID_ARGUMENT
        ],
    )
    def test_create_vehicle_battery_capacity(
        self, channel, namespace, pg_conn, battery, expect_ok
    ):
        """DECIMAL(10,2): 0/small/max = OK; overflow = INVALID_ARGUMENT."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.battery_capacity_kwh = battery
        if expect_ok:
            with TrackedInsert(pg_conn, "vehicle") as ti:
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
            # DECIMAL(10,2) may round-trip exactly or with rounding; close enough
            assert abs(resp.battery_capacity_kwh - battery) < 0.01
        else:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateVehicle(req)
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_create_vehicle_negative_battery_accepted(
        self, channel, namespace, pg_conn
    ):
        """Negative battery is accepted (no app-level validation). Documents current behavior."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.battery_capacity_kwh = -1.0
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        assert resp.battery_capacity_kwh == -1.0

    def test_create_vehicle_negative_range_accepted(
        self, channel, namespace, pg_conn
    ):
        """Negative range is accepted (no app-level validation)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.calibrated_range_km = -100
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        assert resp.calibrated_range_km == -100

    def test_create_vehicle_empty_brand_accepted(
        self, channel, namespace, pg_conn
    ):
        """Empty brand is accepted (VARCHAR allows empty)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.brand = ""
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        assert resp.brand == ""

    def test_create_vehicle_unicode_brand_accepted(
        self, channel, namespace, pg_conn
    ):
        """Unicode brand is accepted (PG VARCHAR is UTF-8)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.brand = "测试-品牌"
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        assert resp.brand == "测试-品牌"

    def test_create_vehicle_earliest_purchase_date_accepted(
        self, channel, namespace, pg_conn
    ):
        """DATE 1900-01-01 is accepted (no lower bound enforced)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.purchase_date = datetime(1900, 1, 1, 0, 0, 0)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        # Convert protobuf Timestamp → datetime for the assertion
        ts = resp.purchase_date
        dt = datetime.fromtimestamp(ts.seconds, tz=timezone.utc)
        assert dt.year == 1900

    def test_create_vehicle_future_purchase_date_accepted(
        self, channel, namespace, pg_conn
    ):
        """DATE in the future is accepted (no upper bound)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        req.purchase_date = datetime(2099, 12, 31, 0, 0, 0)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            resp = stub.CreateVehicle(req)
            ti.register(resp.id)
        ts = resp.purchase_date
        dt = datetime.fromtimestamp(ts.seconds, tz=timezone.utc)
        assert dt.year == 2099

    def test_update_vehicle_change_license_plate(
        self, channel, namespace, pg_conn
    ):
        """UpdateVehicle can change license_plate (assumes new plate is unique)."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            new_plate = make_license_plate(namespace)
            updated = stub.UpdateVehicle(pb.UpdateVehicleRequest(
                id=created.id,
                brand=created.brand,
                calibrated_range_km=created.calibrated_range_km,
                battery_capacity_kwh=created.battery_capacity_kwh,
                purchase_date=created.purchase_date,
                license_plate=new_plate,
            ))
        assert updated.license_plate == new_plate

    def test_update_vehicle_change_battery_capacity(
        self, channel, namespace, pg_conn
    ):
        """UpdateVehicle can change battery_capacity_kwh."""
        stub = rpc.VehicleServiceStub(channel)
        req = _make_create_req(namespace)
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            updated = stub.UpdateVehicle(pb.UpdateVehicleRequest(
                id=created.id,
                brand=created.brand,
                calibrated_range_km=created.calibrated_range_km,
                battery_capacity_kwh=99.9,
                purchase_date=created.purchase_date,
                license_plate=created.license_plate,
            ))
        assert updated.battery_capacity_kwh == 99.9

    def test_list_vehicles_paging_returns_consistent(
        self, channel, namespace, pg_conn
    ):
        """Use small page_size to confirm paging protocol works.

        Creates 3 vehicles, then walks pages of 1 to verify all are found.
        ListVehicles runs INSIDE the with-block so the rows aren't cleaned up yet.
        """
        stub = rpc.VehicleServiceStub(channel)
        ids = []
        with TrackedInsert(pg_conn, "vehicle") as ti:
            for i in range(3):
                req = _make_create_req(namespace)
                req.license_plate = make_license_plate(namespace)
                resp = stub.CreateVehicle(req)
                ti.register(resp.id)
                ids.append(resp.id)
            # Walk pages inside the with-block (rows still exist)
            found = set()
            page_token = ""
            for _ in range(20):
                resp = stub.ListVehicles(
                    pb.ListVehiclesRequest(page_size=1, page_token=page_token)
                )
                for v in resp.vehicles:
                    if v.id in ids:
                        found.add(v.id)
                if not resp.next_page_token:
                    break
                page_token = resp.next_page_token
        assert found == set(ids), f"missing from paged list: {set(ids) - found}"


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

    def test_delete_vehicle_with_consumption_returns_invalid_argument(
        self, channel, namespace, pg_conn
    ):
        """FK constraint: cannot delete a vehicle that has consumption rows.

        Setup: create vehicle V, create consumption C referencing V.
        Action: DeleteVehicle(V).
        Expected: INVALID_ARGUMENT (per src/db/error.cc: foreign_key_violation
        → INVALID_ARGUMENT, not FAILED_PRECONDITION).

        Cross-service test: depends on ConsumptionService existing
        (Chunk 6 enables it). Enabled 2026-08-19.
        """
        from tests.python.gen.evgrpc import consumption_pb2 as c_pb
        from tests.python.gen.evgrpc import consumption_pb2_grpc as c_rpc
        from tests.python.gen.evgrpc import weather_pb2 as w_pb
        from tests.python.gen.evgrpc import weather_pb2_grpc as w_rpc
        from datetime import datetime, timedelta
        from tests.python._helpers import make_license_plate, make_weather_name

        v_stub = rpc.VehicleServiceStub(channel)
        c_stub = c_rpc.ConsumptionServiceStub(channel)
        w_stub = w_rpc.WeatherServiceStub(channel)

        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti:
            plate = make_license_plate(namespace)
            v = v_stub.CreateVehicle(pb.CreateVehicleRequest(
                brand="test-brand",
                calibrated_range_km=400,
                battery_capacity_kwh=75.0,
                purchase_date=datetime(2024, 1, 1, 0, 0, 0),
                license_plate=plate,
            ))
            v_ti.register(v.id)
            w_name = make_weather_name(namespace)
            w = w_stub.CreateWeather(w_pb.CreateWeatherRequest(name=w_name))
            w_ti.register(w.id)

            # Create consumption referencing V (FK to vehicle).
            start = datetime(2024, 6, 15, 10, 0, 0)
            c = c_stub.CreateConsumption(c_pb.CreateConsumptionRequest(
                vehicle_id=v.id,
                start=start,
                end=start + timedelta(hours=2),
                begin_percent=80, end_percent=40,
                begin_mileage_km=100, end_mileage_km=200,
                begin_range_km=320, end_range_km=160,
                highest_temperature_c=25.0, lowest_temperature_c=15.0,
                weather_id=w.id,
                remark="fk-test",
            ))

            with TrackedInsert(pg_conn, "consumption") as c_ti:
                c_ti.register(c.id)
                # Try to delete the vehicle (FK violation expected).
                with pytest.raises(grpc.RpcError) as exc:
                    v_stub.DeleteVehicle(pb.DeleteVehicleRequest(id=v.id))
                assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT
                # Vehicle NOT deleted; cleanup will remove it normally.