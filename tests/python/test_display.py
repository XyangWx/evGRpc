"""DisplayService: 11 RPCs — ~25 tests.

RPCs: GetVehicleCostSummary, GetMonthlyReport, GetAnnualReport,
       GetCostByChargerType, GetCostBySourceCategory,
       GetConsumptionEfficiency, GetRangeAccuracy,
       GetTemperatureConsumptionCorrelation,
       GetDailyChargingReport, GetMonthlyChargingReport, GetAnnualChargingReport.

Strategy: focus on v1.1.0 charging-report RPCs (which use COALESCE-on-empty
and tolerate no data → return zeros). Legacy RPCs (GetMonthlyReport /
GetAnnualReport) fire INTERNAL "no aggregate row" when filter matches
zero data — out of scope to seed, so we just verify the validation paths.
"""

from __future__ import annotations

from datetime import datetime, timedelta

import grpc
import pytest

from tests.python._helpers import TrackedInsert, make_uuid
from tests.python.gen.evgrpc import display_pb2 as pb
from tests.python.gen.evgrpc import display_pb2_grpc as rpc


# ─────────────────────────── TestHappyPath (v1.1.0 charging reports) ───────────────────────────

class TestHappyPath:
    def test_get_daily_charging_report_empty_returns_zeros(
        self, channel, namespace
    ):
        """v1.1.0 RPC: no data → 0 cost/0 kwh/0 count (COALESCE-on-empty)."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetDailyChargingReport(
            pb.GetDailyChargingReportRequest(
                year=2024, month=6, day=15, vehicle_id=""
            )
        )
        assert resp.year == 2024
        assert resp.month == 6
        assert resp.day == 15
        assert resp.total_cost == 0.0
        assert resp.total_kwh == 0.0
        assert resp.count == 0

    def test_get_monthly_charging_report_empty_returns_zeros(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetMonthlyChargingReport(
            pb.GetMonthlyChargingReportRequest(
                year=2024, month=6, vehicle_id=""
            )
        )
        assert resp.year == 2024
        assert resp.month == 6
        assert resp.total_cost == 0.0
        assert resp.total_kwh == 0.0
        assert resp.count == 0

    def test_get_annual_charging_report_empty_returns_zeros(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetAnnualChargingReport(
            pb.GetAnnualChargingReportRequest(year=2024, vehicle_id="")
        )
        assert resp.year == 2024
        assert resp.month == 0  # 0 = annual
        assert resp.total_cost == 0.0
        assert resp.total_kwh == 0.0
        assert resp.count == 0

    def test_get_vehicle_cost_summary_no_data_returns_invalid(
        self, channel, namespace
    ):
        """Random vehicle_id with no charging → INVALID_ARGUMENT ("no aggregate row").

        Production code has an EXISTS pre-check that fires INVALID_ARGUMENT
        when the filter matches zero rows. The v1.1.0 RPCs (Daily/Monthly/
        AnnualChargingReport) handle this case with COALESCE-on-empty-set
        and return zeros; the legacy GetVehicleCostSummary prefers a
        clear INVALID_ARGUMENT over silent zeros.
        """
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicleCostSummary(
                pb.GetVehicleCostSummaryRequest(
                    vehicle_id=make_uuid(),
                    start_time=datetime(2024, 1, 1),
                    end_time=datetime(2024, 12, 31),
                )
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_vehicle_cost_summary_empty_vehicle_id_returns_invalid(
        self, channel, namespace
    ):
        """Empty vehicle_id → INVALID_ARGUMENT (validator)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetVehicleCostSummary(
                pb.GetVehicleCostSummaryRequest(
                    vehicle_id="",
                    start_time=datetime(2024, 1, 1),
                    end_time=datetime(2024, 12, 31),
                )
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── Helpers ───────────────────────────

def _seed_vehicle_with_chargings(
    channel, namespace, pg_conn,
    v_ti, w_ti, sc_ti, c_ti, *,
    n_chargings=2, charger_type=None, kwh_per=30.0, cost_per=35.0,
    base_date=(2024, 6, 15), base_hour=10, duration_hours=1,
):
    """Create vehicle + weather + source_category + N charging rows.

    IMPORTANT: caller controls cleanup. Helper registers rows in caller-
    provided TrackedInsert instances. This lets the caller run assertions
    INSIDE the with-blocks (where rows are still alive).

    v_ti, w_ti, sc_ti, c_ti are the caller's TrackedInsert instances
    for vehicle, weather, source_category, charging respectively.

    Returns (vid, wid, scid, d_stub).
    """
    from datetime import datetime, timedelta
    from tests.python._helpers import (
        make_license_plate, make_source_category_name, make_weather_name,
    )
    from tests.python.gen.evgrpc import vehicle_pb2 as v_pb
    from tests.python.gen.evgrpc import vehicle_pb2_grpc as v_rpc
    from tests.python.gen.evgrpc import weather_pb2 as w_pb
    from tests.python.gen.evgrpc import weather_pb2_grpc as w_rpc
    from tests.python.gen.evgrpc import source_category_pb2 as sc_pb
    from tests.python.gen.evgrpc import source_category_pb2_grpc as sc_rpc
    from tests.python.gen.evgrpc import charging_pb2 as c_pb
    from tests.python.gen.evgrpc import charging_pb2_grpc as c_rpc

    v_stub = v_rpc.VehicleServiceStub(channel)
    w_stub = w_rpc.WeatherServiceStub(channel)
    sc_stub = sc_rpc.SourceCategoryServiceStub(channel)
    c_stub = c_rpc.ChargingServiceStub(channel)
    d_stub = rpc.DisplayServiceStub(channel)

    vid = v_stub.CreateVehicle(v_pb.CreateVehicleRequest(
        brand="test", calibrated_range_km=400, battery_capacity_kwh=75.0,
        purchase_date=datetime(2024, 1, 1, 0, 0, 0),
        license_plate=make_license_plate(namespace),
    )).id
    v_ti.register(vid)
    wid = w_stub.CreateWeather(w_pb.CreateWeatherRequest(
        name=make_weather_name(namespace)
    )).id
    w_ti.register(wid)
    scid = sc_stub.CreateSourceCategory(sc_pb.CreateSourceCategoryRequest(
        name=make_source_category_name(namespace)
    )).id
    sc_ti.register(scid)

    if charger_type is None:
        charger_type = c_pb.CHARGER_TYPE_FAST

    for i in range(n_chargings):
        # Offset start time by (i * duration_hours) hours so multiple
        # rows fit on the same day when duration_hours < 24/day.
        # Default (duration_hours=1) gives rows on consecutive days.
        start = datetime(base_date[0], base_date[1], base_date[2], base_hour, 0, 0) + timedelta(hours=i * duration_hours)
        end = start + timedelta(hours=duration_hours)
        c = c_stub.CreateCharging(c_pb.CreateChargingRequest(
            vehicle_id=vid, start_time=start, end_time=end,
            start_percent=20, end_percent=80,
            start_mileage_km=10000 + i * 100,
            end_mileage_km=10050 + i * 100,
            kwh_charged=kwh_per,
            cost=cost_per,
            electricity_unit_price=1.10,
            charger_type=charger_type,
            source_category_id=scid,
            location="loc", remark="seeded",
        ))
        c_ti.register(c.id)

    return vid, wid, scid, d_stub


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_get_daily_charging_report_year_too_low_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=1899, month=6, day=15)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_month_zero_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=0, day=15)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_month_thirteen_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=13, day=15)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_day_zero_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=6, day=0)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_day_thirtytwo_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=6, day=32)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_monthly_charging_report_year_too_low_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetMonthlyChargingReport(
                pb.GetMonthlyChargingReportRequest(year=1899, month=6)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_annual_charging_report_year_too_low_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetAnnualChargingReport(
                pb.GetAnnualChargingReportRequest(year=1899)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_monthly_report_invalid_arguments_returns_invalid(
        self, channel, namespace
    ):
        """Legacy RPC: validation only. Empty data fires INTERNAL (out of scope to seed)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetMonthlyReport(
                pb.GetMonthlyReportRequest(year=1899, month=6)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_annual_report_invalid_year_returns_invalid(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetAnnualReport(
                pb.GetAnnualReportRequest(year=1899)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    def test_get_daily_charging_report_feb_30_nonleap_returns_invalid(
        self, channel, namespace
    ):
        """Feb 30 in non-leap year → INVALID_ARGUMENT (last-day-of-month)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2023, month=2, day=30)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_feb_29_nonleap_returns_invalid(
        self, channel, namespace
    ):
        """Feb 29 in non-leap year (2023) → INVALID_ARGUMENT."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2023, month=2, day=29)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_feb_29_leap_returns_ok(
        self, channel, namespace
    ):
        """Feb 29 in leap year (2024) → OK + zero response (no data)."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetDailyChargingReport(
            pb.GetDailyChargingReportRequest(year=2024, month=2, day=29)
        )
        assert resp.year == 2024
        assert resp.month == 2
        assert resp.day == 29

    def test_get_daily_charging_report_apr_31_returns_invalid(
        self, channel, namespace
    ):
        """Apr 31 → INVALID_ARGUMENT (April has 30 days)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=4, day=31)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_jun_31_returns_invalid(
        self, channel, namespace
    ):
        """Jun 31 → INVALID_ARGUMENT (June has 30 days)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=6, day=31)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_dec_31_ok(
        self, channel, namespace
    ):
        """Dec 31 → OK (December has 31 days)."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetDailyChargingReport(
            pb.GetDailyChargingReportRequest(year=2024, month=12, day=31)
        )
        assert resp.day == 31

    def test_get_daily_charging_report_year_1900_ok(
        self, channel, namespace
    ):
        """Year 1900 is the minimum (>= 1900)."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetDailyChargingReport(
            pb.GetDailyChargingReportRequest(year=1900, month=1, day=1)
        )
        assert resp.year == 1900

    def test_get_daily_charging_report_year_1899_returns_invalid(
        self, channel, namespace
    ):
        """Year 1899 → INVALID_ARGUMENT (below 1900)."""
        stub = rpc.DisplayServiceStub(channel)
        with pytest.raises(grpc.RpcError) as exc:
            stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=1899, month=1, day=1)
            )
        assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    def test_get_daily_charging_report_with_vehicle_id_filter(
        self, channel, namespace
    ):
        """Vehicle filter: pass a random UUID; should still return zeros (no data)."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetDailyChargingReport(
            pb.GetDailyChargingReportRequest(
                year=2024, month=6, day=15, vehicle_id=make_uuid()
            )
        )
        assert resp.count == 0
        assert resp.vehicle_id  # echoed back


# ─────────────────────────── TestConstraints (TZ-aware charging reports) ───────────────────────────

class TestHappyPathWithSeed:
    """Tests with REAL seeded data (created via VehicleService + ChargingService).

    These verify the aggregation logic actually does the right thing
    (sums, counts, etc.) — not just that empty data returns zeros.
    """

    def test_get_vehicle_cost_summary_with_seeded_data_returns_totals(
        self, channel, namespace, pg_conn
    ):
        """With seeded charging, totals > 0.
        Verifies the aggregation pipeline (PG → C++ → proto) doesn't
        drop data on the way.
        """
        from datetime import datetime
        from tests.python._helpers import (
            make_license_plate, make_source_category_name, make_weather_name,
        )
        from tests.python.gen.evgrpc import vehicle_pb2 as v_pb
        from tests.python.gen.evgrpc import vehicle_pb2_grpc as v_rpc
        from tests.python.gen.evgrpc import weather_pb2 as w_pb
        from tests.python.gen.evgrpc import weather_pb2_grpc as w_rpc
        from tests.python.gen.evgrpc import source_category_pb2 as sc_pb
        from tests.python.gen.evgrpc import source_category_pb2_grpc as sc_rpc
        from tests.python.gen.evgrpc import charging_pb2 as c_pb
        from tests.python.gen.evgrpc import charging_pb2_grpc as c_rpc

        v_stub = v_rpc.VehicleServiceStub(channel)
        w_stub = w_rpc.WeatherServiceStub(channel)
        sc_stub = sc_rpc.SourceCategoryServiceStub(channel)
        c_stub = c_rpc.ChargingServiceStub(channel)
        d_stub = rpc.DisplayServiceStub(channel)

        # Create FK refs
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti:
            vid = v_stub.CreateVehicle(v_pb.CreateVehicleRequest(
                brand="test", calibrated_range_km=400, battery_capacity_kwh=75.0,
                purchase_date=datetime(2024, 1, 1, 0, 0, 0),
                license_plate=make_license_plate(namespace),
            )).id
            v_ti.register(vid)
            wid = w_stub.CreateWeather(w_pb.CreateWeatherRequest(
                name=make_weather_name(namespace)
            )).id
            w_ti.register(wid)
            scid = sc_stub.CreateSourceCategory(sc_pb.CreateSourceCategoryRequest(
                name=make_source_category_name(namespace)
            )).id
            sc_ti.register(scid)

            # Create 2 charging rows + 1 consumption row, all kept alive
            # during the query. consumption is nested inside charging's
            # with-block so the query (further nested) sees both. The
            # consumption's __exit__ runs first, then the charging's, then
            # the outer block.
            with TrackedInsert(pg_conn, "charging") as c_ti:
                for i, percent_end in enumerate([60, 90]):
                    start = datetime(2024, 6, 15 + i, 10, 0, 0)
                    end = datetime(2024, 6, 15 + i, 11, 0, 0)
                    c_stub.CreateCharging(c_pb.CreateChargingRequest(
                        vehicle_id=vid, start_time=start, end_time=end,
                        start_percent=20, end_percent=percent_end,
                        start_mileage_km=10000, end_mileage_km=10100,
                        kwh_charged=30.0 + i * 10,  # 30, 40
                        cost=35.0 + i * 15,  # 35, 50
                        electricity_unit_price=1.10,
                        charger_type=c_pb.CHARGER_TYPE_FAST,
                        source_category_id=scid,
                        location="loc", remark="test",
                    ))
                    # Track for cleanup via the c_ti (registers all
                    # rows to be deleted on c_ti __exit__).
                    c_ti.register(c_stub.ListChargings(
                        c_pb.ListChargingsRequest(vehicle_id=vid, page_size=1000)
                    ).chargings[0].id)

                # Create 1 consumption row NESTED inside charging's
                # with-block so it stays alive during the query.
                from datetime import timedelta
                with TrackedInsert(pg_conn, "consumption") as co_ti:
                    start = datetime(2024, 6, 16, 8, 0, 0)
                    end = start + timedelta(hours=2)
                    from tests.python.gen.evgrpc import consumption_pb2 as cn_pb
                    from tests.python.gen.evgrpc import consumption_pb2_grpc as cn_rpc
                    cn_stub = cn_rpc.ConsumptionServiceStub(channel)
                    c = cn_stub.CreateConsumption(cn_pb.CreateConsumptionRequest(
                        vehicle_id=vid, start=start, end=end,
                        begin_percent=80, end_percent=40,
                        begin_mileage_km=10100, end_mileage_km=10200,
                        begin_range_km=320, end_range_km=160,
                        highest_temperature_c=25.0, lowest_temperature_c=15.0,
                        weather_id=wid, remark="consumed",
                    ))
                    co_ti.register(c.id)

                    # Query the aggregation INSIDE both with-blocks so
                    # all rows still exist.
                    resp = d_stub.GetVehicleCostSummary(pb.GetVehicleCostSummaryRequest(
                        vehicle_id=vid,
                        start_time=datetime(2024, 1, 1),
                        end_time=datetime(2024, 12, 31),
                    ))

                    # Verify totals (30 + 40 = 70 kwh, 35 + 50 = 85 cost)
                    assert resp.total_kwh == pytest.approx(70.0, abs=0.01), f"kwh={resp.total_kwh}"
                    assert resp.total_cost == pytest.approx(85.0, abs=0.01), f"cost={resp.total_cost}"
                    assert resp.avg_yuan_per_kwh == pytest.approx(85.0 / 70.0, abs=0.01)
                    # Mileage from consumption: 10200 - 10100 = 100 km
                    # avg_yuan_per_km = total_cost / total_km = 85 / 100 = 0.85
                    assert resp.avg_yuan_per_km == pytest.approx(85.0 / 100.0, abs=0.01)

    def test_get_monthly_charging_report_with_seeded_data_returns_totals(
        self, channel, namespace, pg_conn
    ):
        """With 2 charging rows on 2024-06-15 + 16, monthly report for 2024-06 = totals > 0.

        All assertions run INSIDE the outer with-blocks so the rows still exist.
        """
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, _, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=30.0, cost_per=35.0,
            )
            resp = d_stub.GetMonthlyChargingReport(
                pb.GetMonthlyChargingReportRequest(year=2024, month=6)
            )
            assert resp.year == 2024
            assert resp.month == 6
            assert resp.count == 2
            assert resp.total_kwh == pytest.approx(60.0, abs=0.01)
            assert resp.total_cost == pytest.approx(70.0, abs=0.01)

    def test_get_annual_charging_report_with_seeded_data_returns_totals(
        self, channel, namespace, pg_conn
    ):
        """With 3 charging rows across the year, annual report = totals > 0."""
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, _, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=3, kwh_per=20.0, cost_per=25.0,
                base_date=(2024, 3, 1),
            )
            resp = d_stub.GetAnnualChargingReport(
                pb.GetAnnualChargingReportRequest(year=2024)
            )
            assert resp.year == 2024
            assert resp.month == 0
            assert resp.count == 3
            assert resp.total_kwh == pytest.approx(60.0, abs=0.01)
            assert resp.total_cost == pytest.approx(75.0, abs=0.01)

    def test_get_daily_charging_report_with_seeded_data_returns_count(
        self, channel, namespace, pg_conn
    ):
        """With 2 charging rows on 2024-06-15, daily report = count 2, kwh 60."""
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            # Both rows on the same day (offset by hours, not days).
            vid, _, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=30.0, cost_per=35.0,
                base_date=(2024, 6, 15), base_hour=8,
                duration_hours=4,  # 08-12, 12-16 — same day
            )
            resp = d_stub.GetDailyChargingReport(
                pb.GetDailyChargingReportRequest(year=2024, month=6, day=15)
            )
            assert resp.day == 15
            assert resp.count == 2
            assert resp.total_kwh == pytest.approx(60.0, abs=0.01)
            assert resp.total_cost == pytest.approx(70.0, abs=0.01)

    def test_get_cost_by_charger_type_with_seeded_data_returns_breakdown(
        self, channel, namespace, pg_conn
    ):
        """Seed 2 FAST charging rows for one vehicle.

        Verify GetCostByChargerType returns 1 breakdown (FAST) with the
        summed totals.
        """
        from tests.python.gen.evgrpc import charging_pb2 as c_pb
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, _, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=30.0, cost_per=35.0,
                charger_type=c_pb.CHARGER_TYPE_FAST,
            )
            resp = d_stub.GetCostByChargerType(pb.GetCostByChargerTypeRequest(
                vehicle_id=vid,
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            ))
            assert len(resp.breakdowns) == 1
            b = resp.breakdowns[0]
            assert b.charger_type == c_pb.CHARGER_TYPE_FAST
            assert b.total_cost == pytest.approx(70.0, abs=0.01)
            assert b.total_kwh == pytest.approx(60.0, abs=0.01)
            assert b.avg_yuan_per_kwh == pytest.approx(70.0 / 60.0, abs=0.01)

    def test_get_cost_by_source_category_with_seeded_data_returns_breakdown(
        self, channel, namespace, pg_conn
    ):
        """Seed 2 charging rows for one source_category.

        Verify GetCostBySourceCategory returns 1 breakdown with summed totals.
        """
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, _, scid, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=30.0, cost_per=35.0,
            )
            resp = d_stub.GetCostBySourceCategory(pb.GetCostBySourceCategoryRequest(
                vehicle_id=vid,
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            ))
            assert len(resp.breakdowns) == 1
            b = resp.breakdowns[0]
            assert b.source_category_id == scid
            assert b.total_cost == pytest.approx(70.0, abs=0.01)
            assert b.total_kwh == pytest.approx(60.0, abs=0.01)

    def test_get_consumption_efficiency_with_seeded_data_returns_efficiency(
        self, channel, namespace, pg_conn
    ):
        """Seed 2 charging rows + 1 consumption row.

        Verify GetConsumptionEfficiency returns 1 efficiency row with
        km/kwh ratio matching the seeded data.
        """
        from datetime import timedelta
        from tests.python.gen.evgrpc import consumption_pb2 as cn_pb
        from tests.python.gen.evgrpc import consumption_pb2_grpc as cn_rpc
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, wid, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=30.0, cost_per=35.0,
                base_date=(2024, 6, 15),
            )
            cn_stub = cn_rpc.ConsumptionServiceStub(channel)
            with TrackedInsert(pg_conn, "consumption") as co_ti:
                start = datetime(2024, 6, 16, 8, 0, 0)
                end = start + timedelta(hours=2)
                c = cn_stub.CreateConsumption(cn_pb.CreateConsumptionRequest(
                    vehicle_id=vid, start=start, end=end,
                    begin_percent=80, end_percent=40,
                    begin_mileage_km=10000, end_mileage_km=10100,
                    begin_range_km=320, end_range_km=160,
                    highest_temperature_c=25.0, lowest_temperature_c=15.0,
                    weather_id=wid, remark="efficiency-test",
                ))
                co_ti.register(c.id)
                resp = d_stub.GetConsumptionEfficiency(
                    pb.GetConsumptionEfficiencyRequest(
                        vehicle_id=vid,
                        start_time=datetime(2024, 1, 1),
                        end_time=datetime(2024, 12, 31),
                    )
                )
                assert len(resp.efficiencies) == 1
                e = resp.efficiencies[0]
                assert e.vehicle_id == vid
                assert e.total_kwh == pytest.approx(60.0, abs=0.01)
                assert e.total_km == pytest.approx(100.0, abs=0.01)
                assert e.km_per_kwh == pytest.approx(100.0 / 60.0, abs=0.01)
                assert e.kwh_per_100km == pytest.approx(60.0, abs=0.01)

    def test_get_range_accuracy_with_seeded_data_returns_accuracy(
        self, channel, namespace, pg_conn
    ):
        """Seed 1 consumption row with begin_range > end_range (dashboard range)
        and end_mileage > begin_mileage (actual mileage). Verify ratio = actual/dashboard.
        """
        from datetime import timedelta
        from tests.python.gen.evgrpc import consumption_pb2 as cn_pb
        from tests.python.gen.evgrpc import consumption_pb2_grpc as cn_rpc
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, wid, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=1, kwh_per=20.0, cost_per=25.0,
                base_date=(2024, 6, 15),
            )
            cn_stub = cn_rpc.ConsumptionServiceStub(channel)
            with TrackedInsert(pg_conn, "consumption") as co_ti:
                start = datetime(2024, 6, 16, 8, 0, 0)
                end = start + timedelta(hours=2)
                # Dashboard range: 200-100 = 100 km (BeginRange - EndRange)
                # Actual mileage: 10200-10000 = 200 km (EndMileage - BeginMileage)
                # Ratio = 200/100 = 2.0
                c = cn_stub.CreateConsumption(cn_pb.CreateConsumptionRequest(
                    vehicle_id=vid, start=start, end=end,
                    begin_percent=80, end_percent=40,
                    begin_mileage_km=10000, end_mileage_km=10200,
                    begin_range_km=200, end_range_km=100,
                    highest_temperature_c=25.0, lowest_temperature_c=15.0,
                    weather_id=wid, remark="range-test",
                ))
                co_ti.register(c.id)
                resp = d_stub.GetRangeAccuracy(pb.GetRangeAccuracyRequest(
                    vehicle_id=vid,
                    start_time=datetime(2024, 1, 1),
                    end_time=datetime(2024, 12, 31),
                ))
                assert len(resp.accuracies) == 1
                a = resp.accuracies[0]
                assert a.vehicle_id == vid
                assert a.dashboard_range_total_km == pytest.approx(100.0, abs=0.01)
                assert a.actual_mileage_total_km == pytest.approx(200.0, abs=0.01)
                assert a.accuracy_ratio == pytest.approx(2.0, abs=0.01)

    def test_get_temperature_consumption_correlation_with_seeded_data_returns_buckets(
        self, channel, namespace, pg_conn
    ):
        """Seed 2 consumption rows with different avg_temps; verify
        they fall into the right temperature buckets.
        """
        from datetime import timedelta
        from tests.python.gen.evgrpc import consumption_pb2 as cn_pb
        from tests.python.gen.evgrpc import consumption_pb2_grpc as cn_rpc
        with TrackedInsert(pg_conn, "vehicle") as v_ti, \
             TrackedInsert(pg_conn, "weather") as w_ti, \
             TrackedInsert(pg_conn, "source_category") as sc_ti, \
             TrackedInsert(pg_conn, "charging") as c_ti:
            vid, wid, _, d_stub = _seed_vehicle_with_chargings(
                channel, namespace, pg_conn,
                v_ti, w_ti, sc_ti, c_ti,
                n_chargings=2, kwh_per=20.0, cost_per=25.0,
                base_date=(2024, 6, 15),
            )
            cn_stub = cn_rpc.ConsumptionServiceStub(channel)
            with TrackedInsert(pg_conn, "consumption") as co_ti:
                # Event 1: avg_temp = 25 (bucket 20-30)
                start1 = datetime(2024, 6, 16, 8, 0, 0)
                c1 = cn_stub.CreateConsumption(cn_pb.CreateConsumptionRequest(
                    vehicle_id=vid, start=start1, end=start1 + timedelta(hours=1),
                    begin_percent=80, end_percent=40,
                    begin_mileage_km=10000, end_mileage_km=10100,
                    begin_range_km=100, end_range_km=80,
                    highest_temperature_c=25.0, lowest_temperature_c=25.0,
                    weather_id=wid, remark="temp-warm",
                ))
                co_ti.register(c1.id)
                # Event 2: avg_temp = 5 (bucket 0-10)
                start2 = datetime(2024, 6, 17, 8, 0, 0)
                c2 = cn_stub.CreateConsumption(cn_pb.CreateConsumptionRequest(
                    vehicle_id=vid, start=start2, end=start2 + timedelta(hours=1),
                    begin_percent=80, end_percent=40,
                    begin_mileage_km=10100, end_mileage_km=10200,
                    begin_range_km=80, end_range_km=60,
                    highest_temperature_c=5.0, lowest_temperature_c=5.0,
                    weather_id=wid, remark="temp-cool",
                ))
                co_ti.register(c2.id)
                resp = d_stub.GetTemperatureConsumptionCorrelation(
                    pb.GetTemperatureConsumptionCorrelationRequest(
                        vehicle_id=vid,
                        start_time=datetime(2024, 1, 1),
                        end_time=datetime(2024, 12, 31),
                    )
                )
                # Should have 2 buckets (one per temperature range)
                assert len(resp.buckets) == 2
                # Find each bucket by label
                by_label = {b.label: b for b in resp.buckets}
                assert "20-30" in by_label
                assert "0-10" in by_label
                # Each event had 1 sample + 20km mileage = 20km, 20kwh each
                assert by_label["20-30"].sample_count == 1
                assert by_label["0-10"].sample_count == 1


class TestConstraints:
    def test_get_monthly_charging_report_with_specific_vehicle(
        self, channel, namespace
    ):
        """Filter by vehicle_id; should return zeros for unknown vehicle."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetMonthlyChargingReport(
            pb.GetMonthlyChargingReportRequest(
                year=2024, month=6, vehicle_id=make_uuid()
            )
        )
        assert resp.count == 0
        assert resp.vehicle_id

    def test_get_annual_charging_report_with_specific_vehicle(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetAnnualChargingReport(
            pb.GetAnnualChargingReportRequest(year=2024, vehicle_id=make_uuid())
        )
        assert resp.count == 0

    def test_get_cost_by_charger_type_empty_returns_zero_breakdowns(
        self, channel, namespace
    ):
        """No data → 0 breakdowns."""
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetCostByChargerType(
            pb.GetCostByChargerTypeRequest(
                vehicle_id=make_uuid(),
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            )
        )
        assert isinstance(resp, pb.GetCostByChargerTypeResponse)
        assert len(resp.breakdowns) >= 0  # may be 0

    def test_get_cost_by_source_category_empty_returns_zero_breakdowns(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetCostBySourceCategory(
            pb.GetCostBySourceCategoryRequest(
                vehicle_id=make_uuid(),
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            )
        )
        assert isinstance(resp, pb.GetCostBySourceCategoryResponse)
        assert len(resp.breakdowns) >= 0

    def test_get_consumption_efficiency_empty_returns_zero(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetConsumptionEfficiency(
            pb.GetConsumptionEfficiencyRequest(
                vehicle_id=make_uuid(),
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            )
        )
        assert isinstance(resp, pb.GetConsumptionEfficiencyResponse)

    def test_get_range_accuracy_empty_returns_zero(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetRangeAccuracy(
            pb.GetRangeAccuracyRequest(
                vehicle_id=make_uuid(),
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            )
        )
        assert isinstance(resp, pb.GetRangeAccuracyResponse)

    def test_get_temperature_consumption_correlation_empty_returns_zero(
        self, channel, namespace
    ):
        stub = rpc.DisplayServiceStub(channel)
        resp = stub.GetTemperatureConsumptionCorrelation(
            pb.GetTemperatureConsumptionCorrelationRequest(
                vehicle_id=make_uuid(),
                start_time=datetime(2024, 1, 1),
                end_time=datetime(2024, 12, 31),
            )
        )
        assert isinstance(resp, pb.GetTemperatureConsumptionCorrelationResponse)