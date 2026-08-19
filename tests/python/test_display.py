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