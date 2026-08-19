# test_display.md

## Overview
- **Service:** DisplayService
- **RPCs:** 11 (3 v1.1.0 charging reports + 8 others)
- **Total tests:** 39 (was 37; +2 added in Phase B for GetRangeAccuracy + GetTemperatureConsumptionCorrelation)
- **Strategy:** Focus on validation paths + empty-data responses for v1.1.0 RPCs (COALESCE-on-empty returns zeros). Legacy RPCs fire INTERNAL on empty data — documented, not seeded.

## TestHappyPath (v1.1.0 charging reports + VehicleCostSummary)

### test_get_daily_charging_report_empty_returns_zeros
- v1.1.0 RPC, COALESCE(SUM, 0) → year=2024, month=6, day=15, all totals 0.

### test_get_monthly_charging_report_empty_returns_zeros
- v1.1.0 RPC → all zeros.

### test_get_annual_charging_report_empty_returns_zeros
- v1.1.0 RPC → all zeros, month=0 (annual indicator).

### test_get_vehicle_cost_summary_no_data_returns_invalid
- **Phase 3 fix:** EXISTS pre-check now fires INVALID_ARGUMENT ("no aggregate row") instead of INTERNAL when filter matches zero rows. The v1.1.0 RPCs (Daily/Monthly/AnnualChargingReport) handle this case with COALESCE-on-empty-set and return zeros; the legacy GetVehicleCostSummary prefers a clear INVALID_ARGUMENT over silent zeros.

### test_get_vehicle_cost_summary_empty_vehicle_id_returns_invalid
- Validator: vehicle_id is required → INVALID_ARGUMENT.

## TestHappyPathWithSeed

### test_get_vehicle_cost_summary_with_seeded_data_returns_totals
- **RPC:** GetVehicleCostSummary with REAL seeded data
- **Purpose:** Verify the aggregation pipeline (PG → C++ → proto) doesn't drop data on the way to the response.
- **Setup:** Create 1 vehicle + 1 weather + 1 source_category + 2 charging rows + 1 consumption row with known values (kwh=30+40=70, cost=35+50=85, mileage delta=100km).
- **Action:** Call GetVehicleCostSummary with the vehicle_id and a date range covering all seeded data.
- **Expected:** total_kwh=70, total_cost=85, avg_yuan_per_kwh=85/70≈1.214, avg_yuan_per_km=85/100=0.85.
- **Critical implementation note:** The query MUST run inside both the `with TrackedInsert(conn, "charging")` and `with TrackedInsert(conn, "consumption")` blocks so neither `__exit__` cleans up the rows before the query reads them. The consumption block is nested inside the charging block so they're all alive at query time.

### test_get_monthly_charging_report_with_seeded_data_returns_totals
- 2 charging rows on 2024-06-15 + 16; query month=2024-06 → count=2, kwh=60, cost=70.

### test_get_annual_charging_report_with_seeded_data_returns_totals
- 3 charging rows across 2024-Q2/Q3; query year=2024 → count=3, kwh=60, cost=75.

### test_get_daily_charging_report_with_seeded_data_returns_count
- 2 charging rows on 2024-06-15 (different hours); query day=2024-06-15 → count=2, kwh=60.

### test_get_cost_by_charger_type_with_seeded_data_returns_breakdown
- 2 FAST charging rows; query → 1 breakdown (FAST) with summed totals.

### test_get_cost_by_source_category_with_seeded_data_returns_breakdown
- 2 charging rows in 1 source_category; query → 1 breakdown with summed totals + source_category_id match.

### test_get_consumption_efficiency_with_seeded_data_returns_efficiency
- 2 charging rows (60 kwh total) + 1 consumption row (100 km); query → km/kwh ≈ 1.667, kwh/100km ≈ 60.

### test_get_range_accuracy_with_seeded_data_returns_accuracy
- 1 consumption with begin_range_km=200, end_range_km=100 (dashboard = 100km),
  end_mileage_km - begin_mileage_km = 200km (actual). Query → accuracy_ratio = 200/100 = 2.0.

### test_get_temperature_consumption_correlation_with_seeded_data_returns_buckets
- 2 consumption rows with different avg_temps (25°C and 5°C); query → 2 buckets ("20-30", "0-10"),
  each with sample_count=1.

## TestErrorPath

### test_get_daily_charging_report_year_too_low_returns_invalid
### test_get_daily_charging_report_month_zero_returns_invalid
### test_get_daily_charging_report_month_thirteen_returns_invalid
### test_get_daily_charging_report_day_zero_returns_invalid
### test_get_daily_charging_report_day_thirtytwo_returns_invalid
### test_get_monthly_charging_report_year_too_low_returns_invalid
### test_get_annual_charging_report_year_too_low_returns_invalid
### test_get_monthly_report_invalid_arguments_returns_invalid
### test_get_annual_report_invalid_year_returns_invalid
- All: validator boundaries → INVALID_ARGUMENT.

## TestBoundaries

### test_get_daily_charging_report_feb_30_nonleap_returns_invalid
- 2023-02-30 → INVALID_ARGUMENT (LastDayOfMonth(2023, 2) = 28).

### test_get_daily_charging_report_feb_29_nonleap_returns_invalid
- 2023-02-29 → INVALID_ARGUMENT.

### test_get_daily_charging_report_feb_29_leap_returns_ok
- 2024-02-29 → OK + zero response.

### test_get_daily_charging_report_apr_31_returns_invalid
- 2024-04-31 → INVALID_ARGUMENT (April has 30 days).

### test_get_daily_charging_report_jun_31_returns_invalid
- 2024-06-31 → INVALID_ARGUMENT (June has 30 days).

### test_get_daily_charging_report_dec_31_ok
- 2024-12-31 → OK (December has 31 days, at limit).

### test_get_daily_charging_report_year_1900_ok
- year=1900 → OK (minimum boundary).

### test_get_daily_charging_report_year_1899_returns_invalid
- year=1899 → INVALID_ARGUMENT (below 1900).

### test_get_daily_charging_report_with_vehicle_id_filter
- Random UUID filter → 0 count, vehicle_id echoed back.

## TestConstraints (TZ-aware + multi-RPC empty responses)

### test_get_monthly_charging_report_with_specific_vehicle
- vehicle_id filter on monthly → 0 count.

### test_get_annual_charging_report_with_specific_vehicle
- vehicle_id filter on annual → 0 count.

### test_get_cost_by_charger_type_empty_returns_zero_breakdowns
- Random vehicle_id → 0 breakdowns (no data).

### test_get_cost_by_source_category_empty_returns_zero_breakdowns
- Random vehicle_id → 0 breakdowns.

### test_get_consumption_efficiency_empty_returns_zero
- Returns response, 0 efficiencies.

### test_get_range_accuracy_empty_returns_zero
- Returns response, 0 accuracies.

### test_get_temperature_consumption_correlation_empty_returns_zero
- Returns response, 0 buckets.

## TZ-awareness notes

The v1.1.0 RPCs use `c.StartTime::date = make_date($1, $2, $3)` and `EXTRACT(YEAR/MONTH FROM c.StartTime) = $1/2` on TIMESTAMPTZ columns. Session TZ comes from PG postmaster side (not libpq client), so test assertions don't depend on client TZ.

Tests use 2024-06-15 (mid-year, far from DST) to avoid TZ boundary ambiguity.

## NOT tested (out of scope per Chunk 7 plan)

- "No data → zero totals" for legacy `GetMonthlyReport`/`GetAnnualReport`/`GetVehicleCostSummary` (production fires INTERNAL via EXISTS pre-check; can't bypass without seeding).
- TZ edge cases (tests use stable mid-year dates).
- Pagination/filtering of multi-RPC responses beyond `vehicle_id` filter.