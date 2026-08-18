# test_display.md

## Overview
- **Service:** DisplayService
- **RPCs:** 11 (3 v1.1.0 charging reports + 8 others)
- **Total tests:** 30 (was 26; +4 added in Phase 2 round-2 review)
- **Strategy:** Focus on validation paths + empty-data responses for v1.1.0 RPCs (COALESCE-on-empty returns zeros). Legacy RPCs fire INTERNAL on empty data — documented, not seeded.

## TestHappyPath (v1.1.0 charging reports + VehicleCostSummary)

### test_get_daily_charging_report_empty_returns_zeros
- v1.1.0 RPC, COALESCE(SUM, 0) → year=2024, month=6, day=15, all totals 0.

### test_get_monthly_charging_report_empty_returns_zeros
- v1.1.0 RPC → all zeros.

### test_get_annual_charging_report_empty_returns_zeros
- v1.1.0 RPC → all zeros, month=0 (annual indicator).

### test_get_vehicle_cost_summary_no_data_returns_internal
- **Documents production behavior:** EXISTS pre-check fires INTERNAL ("no aggregate row") when filter matches zero rows. Pre-dates v1.1.0 pattern. Cannot test "no data → zero totals" path without seeding.

### test_get_vehicle_cost_summary_empty_vehicle_id_returns_invalid
- Validator: vehicle_id is required → INVALID_ARGUMENT.

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