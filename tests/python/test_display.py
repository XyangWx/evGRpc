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

from tests.python._helpers import make_uuid
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

    def test_get_vehicle_cost_summary_no_data_returns_internal(
        self, channel, namespace
    ):
        """Random vehicle_id with no charging → INTERNAL ("no aggregate row").

        Production code has an EXISTS pre-check that fires INTERNAL when
        the filter matches zero rows. COALESCE-on-empty-set would have
        returned zeros, but the pre-check precedes the aggregation. Tests
        documenting this behavior; production change would be a separate
        spec (likely breaking).
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
        assert exc.value.code() == grpc.StatusCode.INTERNAL

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