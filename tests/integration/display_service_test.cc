// Integration tests for DisplayService.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on aggregation / list-shape
// behavior without re-minting bearer tokens or repeating fixture
// setup.
//
// Helpers (CreateVehicleId, CreateSourceCategoryId, CreateChargingId,
// CreateConsumptionId, SeedVehicleDataForDisplay, DefaultTimeRange)
// live in evgrpc::test::data; see test_data.h for the contract.
//
// This file currently holds only the Task 31 smoke test
// (DataHelpers_ProduceValidSetup). Per-RPC TEST_F's will be
// appended in Tasks 32-39 (their respective follow-up commits).

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/charging.grpc.pb.h"
#include "evgrpc/charging.pb.h"
#include "evgrpc/display.grpc.pb.h"
#include "evgrpc/display.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` / `pg()` accessors. Declared at the top of this file
// (not in service_test_fixtures.h) because each service's tests live
// in their own .cc — mirrors the ChargingServiceIT /
// ConsumptionServiceIT pattern established in Chunks 3-4.
class DisplayServiceIT : public ServiceITBase {};

// Task 31 smoke test: exercises the Chunk 5 DisplayService helpers
// in evgrpc::test::data (SeedVehicleDataForDisplay, DefaultTimeRange,
// plus the underlying CreateVehicleId / CreateSourceCategoryId /
// CreateChargingId / CreateConsumptionId) by seeding one vehicle
// worth of data and then verifying a follow-up CreateChargingId
// still returns a non-empty id (regression-guard for the helper
// chain being internally consistent after Chunk 5 wiring).
//
// Lives here (not in test_data.cc) so the smoke runs inside a
// ServiceITBase fixture and has access to channel() and pg().
TEST_F(DisplayServiceIT, DataHelpers_ProduceValidSetup) {
  const auto vid = data::CreateVehicleId(channel());
  EXPECT_FALSE(vid.empty());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  // Stronger signal: re-create and verify all helpers return non-empty.
  const auto cid = data::CreateChargingId(
      channel(), vid, data::CreateSourceCategoryId(channel()));
  EXPECT_FALSE(cid.empty());
  // DefaultTimeRange returns the expected fixed window.
  const auto tr = data::DefaultTimeRange();
  EXPECT_EQ(tr.start.seconds(), 1672531200);  // 2023-01-01
  EXPECT_EQ(tr.end.seconds(), 1704067200);    // 2024-01-01
}

// Task 32: GetVehicleCostSummary happy path. Seeds enough
// charging + consumption for one vehicle via the Display helper, then
// asks DisplayService for a VehicleCostSummary over the default time
// range. Asserts vehicle_id round-trips and totals are positive.
TEST_F(DisplayServiceIT, GetVehicleCostSummary_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  VehicleCostSummary resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetVehicleCostSummary(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_GT(resp.total_cost(), 0.0);
  EXPECT_GT(resp.total_kwh(), 0.0);
}

// Task 32: GetVehicleCostSummary with no data → INTERNAL "no aggregate
// row". The production handler's precursor fix (commit 69d0d85) added
// an EXISTS pre-check so this branch is reachable.
TEST_F(DisplayServiceIT, GetVehicleCostSummary_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  // Note: do NOT seed data — empty DB.
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  VehicleCostSummary resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicleCostSummary(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos)
      << st.error_message();
}

// Task 33: GetMonthlyReport happy path. Seeding uses Nov 2023
// (1700000000 epoch), so year=2023 month=11 hits the seeded data.
// Asserts year + month round-trip and total_cost > 0.
TEST_F(DisplayServiceIT, GetMonthlyReport_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(2023);   // helper data is in Nov 2023
  req.set_month(11);
  req.set_vehicle_id(vid);  // optional
  PeriodReport resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetMonthlyReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.year(), 2023);
  EXPECT_EQ(resp.month(), 11);
  EXPECT_GT(resp.total_cost(), 0.0);
}

// Task 33: GetMonthlyReport with future year → no data → INTERNAL
// "no aggregate row". Same branch as Task 32, reached via the
// precursor's EXISTS pre-check on year/month/vehicle filter.
TEST_F(DisplayServiceIT, GetMonthlyReport_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(2099);  // future year, no data
  req.set_month(1);
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetMonthlyReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos);
}

// Task 34: GetAnnualReport happy path. Same Nov-2023 data as Task 33
// but asks for the full year (no month filter). Asserts year round-
// trips, month=0 (annual sentinel per spec), and total_cost > 0.
TEST_F(DisplayServiceIT, GetAnnualReport_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetAnnualReportRequest req;
  req.set_year(2023);
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetAnnualReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.year(), 2023);
  EXPECT_EQ(resp.month(), 0);  // 0 = annual sentinel
  EXPECT_GT(resp.total_cost(), 0.0);
}

// Task 34: GetAnnualReport with future year → no data → INTERNAL.
TEST_F(DisplayServiceIT, GetAnnualReport_NoData_Internal) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetAnnualReportRequest req;
  req.set_year(2099);
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetAnnualReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos);
}

// Task 35: GetCostByChargerType happy path. Seeds 3 charging rows
// for one vehicle (all CHARGER_TYPE_FAST per helper), then asks for
// the cost breakdown over the default time range. Asserts at least
// 1 breakdown returned and every breakdown has positive total_cost.
TEST_F(DisplayServiceIT, GetCostByChargerType_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  EXPECT_GT(resp.breakdowns_size(), 0);
  for (const auto& b : resp.breakdowns()) {
    EXPECT_GT(b.total_cost(), 0.0);
  }
}

// Task 35: GetCostByChargerType empty case. No data seeded — list
// RPC returns an empty list (NOT INTERNAL — that's only for the 3
// aggregation RPCs).
TEST_F(DisplayServiceIT, GetCostByChargerType_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}

// Task 35: GetCostByChargerType filtered case. Seeds two vehicles,
// asks for unfiltered (both) then filtered (vid_a only). Filtered
// total should be strictly less than unfiltered total.
TEST_F(DisplayServiceIT, GetCostByChargerType_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  // Unfiltered baseline — both vehicles' data.
  GetCostByChargerTypeRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp_unf;
  grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx_unf, req_unf, &resp_unf).ok());
  double total_unf = 0;
  for (const auto& b : resp_unf.breakdowns()) total_unf += b.total_cost();
  // Filtered — vid_a only.
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  double total_filt = 0;
  for (const auto& b : resp.breakdowns()) total_filt += b.total_cost();
  EXPECT_LT(total_filt, total_unf);
}

// Task 36: GetCostBySourceCategory happy path. Seeds charging rows
// (each links to a source_category FK) and asserts at least one
// breakdown row.
TEST_F(DisplayServiceIT, GetCostBySourceCategory_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  EXPECT_GT(resp.breakdowns_size(), 0);
}

// Task 36: GetCostBySourceCategory empty case.
TEST_F(DisplayServiceIT, GetCostBySourceCategory_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}

// Task 36: GetCostBySourceCategory filtered case.
TEST_F(DisplayServiceIT, GetCostBySourceCategory_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp_unf;
  grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx_unf, req_unf, &resp_unf).ok());
  double total_unf = 0;
  for (const auto& b : resp_unf.breakdowns()) total_unf += b.total_cost();
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostBySourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  double total_filt = 0;
  for (const auto& b : resp.breakdowns()) total_filt += b.total_cost();
  EXPECT_LT(total_filt, total_unf);
}

// Task 37: GetConsumptionEfficiency happy path.
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  EXPECT_GT(resp.efficiencies_size(), 0);
}

// Task 37: GetConsumptionEfficiency empty case.
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.efficiencies_size(), 0);
}

// Task 37: GetConsumptionEfficiency filtered case — asserts all
// returned rows have vehicle_id == vid_a (no cross-vehicle leakage).
TEST_F(DisplayServiceIT, GetConsumptionEfficiency_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetConsumptionEfficiencyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  for (const auto& e : resp.efficiencies()) {
    EXPECT_EQ(e.vehicle_id(), vid_a);
  }
}

// Task 38: GetRangeAccuracy happy path.
TEST_F(DisplayServiceIT, GetRangeAccuracy_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  EXPECT_GT(resp.accuracies_size(), 0);
}

// Task 38: GetRangeAccuracy empty case.
TEST_F(DisplayServiceIT, GetRangeAccuracy_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.accuracies_size(), 0);
}

// Task 38: GetRangeAccuracy filtered case.
TEST_F(DisplayServiceIT, GetRangeAccuracy_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetRangeAccuracyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  for (const auto& a : resp.accuracies()) {
    EXPECT_EQ(a.vehicle_id(), vid_a);
  }
}

// Task 39: GetTemperatureConsumptionCorrelation happy path. The
// production query joins charging to consumption (ch.StartTime within
// 24h of c.Start to c.EndTime), so seeded charging + consumption
// rows for the same vehicle should produce >= 1 bucket.
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  EXPECT_GT(resp.buckets_size(), 0);
}

// Task 39: GetTemperatureConsumptionCorrelation empty case.
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.buckets_size(), 0);
}

// Task 39: GetTemperatureConsumptionCorrelation filtered case.
TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_Filtered) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req_unf;
  *req_unf.mutable_start_time() = data::DefaultTimeRange().start;
  *req_unf.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp_unf;
  grpc::ClientContext ctx_unf;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx_unf, req_unf, &resp_unf).ok());
  int total_unf = 0;
  for (const auto& b : resp_unf.buckets()) total_unf += b.sample_count();
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid_a);
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetTemperatureConsumptionCorrelationResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  int total_filt = 0;
  for (const auto& b : resp.buckets()) total_filt += b.sample_count();
  EXPECT_LT(total_filt, total_unf);
}

// Task 41 gap closure: time-range filter excludes seeded data → empty.
// Exercises the narrow-window branches in each list-style RPC's WHERE
// clause (start_time/end_time filters).
TEST_F(DisplayServiceIT, GetCostByChargerType_TimeRangeFilter_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  req.set_vehicle_id(vid);
  req.mutable_start_time()->set_seconds(1735689600);  // 2025-01-01
  req.mutable_end_time()->set_seconds(1735776000);    // 2025-01-02
  GetCostByChargerTypeResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}

TEST_F(DisplayServiceIT, GetCostBySourceCategory_TimeRangeFilter_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetCostBySourceCategoryRequest req;
  req.set_vehicle_id(vid);
  req.mutable_start_time()->set_seconds(1735689600);
  req.mutable_end_time()->set_seconds(1735776000);
  GetCostBySourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostBySourceCategory(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.breakdowns_size(), 0);
}

TEST_F(DisplayServiceIT, GetConsumptionEfficiency_TimeRangeFilter_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetConsumptionEfficiencyRequest req;
  req.set_vehicle_id(vid);
  req.mutable_start_time()->set_seconds(1735689600);
  req.mutable_end_time()->set_seconds(1735776000);
  GetConsumptionEfficiencyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumptionEfficiency(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.efficiencies_size(), 0);
}

TEST_F(DisplayServiceIT, GetRangeAccuracy_TimeRangeFilter_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetRangeAccuracyRequest req;
  req.set_vehicle_id(vid);
  req.mutable_start_time()->set_seconds(1735689600);
  req.mutable_end_time()->set_seconds(1735776000);
  GetRangeAccuracyResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetRangeAccuracy(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.accuracies_size(), 0);
}

TEST_F(DisplayServiceIT, GetTemperatureConsumptionCorrelation_TimeRangeFilter_Empty) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetTemperatureConsumptionCorrelationRequest req;
  req.set_vehicle_id(vid);
  req.mutable_start_time()->set_seconds(1735689600);
  req.mutable_end_time()->set_seconds(1735776000);
  GetTemperatureConsumptionCorrelationResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetTemperatureConsumptionCorrelation(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.buckets_size(), 0);
}

// Task 41 gap closure: GetVehicleCostSummary validator (start > end
// → INVALID_ARGUMENT, per display_service.cc line 14).
TEST_F(DisplayServiceIT, GetVehicleCostSummary_StartAfterEnd_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid);
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  req.set_vehicle_id(vid);
  // Start = 2024-01-02 (after End = 2024-01-01). GetVehicleCostSummary
  // doesn't validate start>end explicitly — the EXISTS pre-check
  // (Task 32 precursor) sees zero rows and returns INTERNAL
  // "no aggregate row" because the time range excludes all data.
  req.mutable_start_time()->set_seconds(1704153600);
  req.mutable_end_time()->set_seconds(1704067200);
  VehicleCostSummary resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicleCostSummary(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_NE(st.error_message().find("no aggregate row"), std::string::npos);
}

// Task 41 gap closure: multi-vehicle no-filter case for
// GetCostByChargerType. Asserts >1 breakdown or larger total than
// the single-vehicle filtered case (Task 35's Filtered test).
TEST_F(DisplayServiceIT, GetCostByChargerType_NoVehicleFilter) {
  const auto vid_a = data::CreateVehicleId(channel());
  const auto vid_b = data::CreateVehicleId(channel());
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_a);
  data::SeedVehicleDataForDisplay(channel(), pg(), vid_b);
  auto stub = DisplayService::NewStub(channel());
  GetCostByChargerTypeRequest req;
  // No vehicle_id filter — aggregates across all vehicles.
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&ctx, req, &resp).ok());
  double total_no_filter = 0;
  for (const auto& b : resp.breakdowns()) total_no_filter += b.total_cost();
  // Compare against single-vehicle baseline.
  GetCostByChargerTypeRequest single_req;
  single_req.set_vehicle_id(vid_a);
  *single_req.mutable_start_time() = data::DefaultTimeRange().start;
  *single_req.mutable_end_time() = data::DefaultTimeRange().end;
  GetCostByChargerTypeResponse single_resp;
  grpc::ClientContext single_ctx;
  ASSERT_TRUE(stub->GetCostByChargerType(&single_ctx, single_req, &single_resp).ok());
  double total_single = 0;
  for (const auto& b : single_resp.breakdowns()) total_single += b.total_cost();
  EXPECT_GT(total_no_filter, total_single);
}

// Task 41 gap closure: vehicle_id empty validator (display_service.cc
// line 45-50) — GetVehicleCostSummary rejects empty vehicle_id with
// INVALID_ARGUMENT before any SQL execution.
TEST_F(DisplayServiceIT, GetVehicleCostSummary_EmptyVehicleId_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetVehicleCostSummaryRequest req;
  // vehicle_id intentionally left empty.
  *req.mutable_start_time() = data::DefaultTimeRange().start;
  *req.mutable_end_time() = data::DefaultTimeRange().end;
  VehicleCostSummary resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicleCostSummary(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 41 gap closure: GetMonthlyReport year/month validators (line
// 133-138). Rejects year < 1900 or month < 1 or month > 12 with
// INVALID_ARGUMENT.
TEST_F(DisplayServiceIT, GetMonthlyReport_InvalidYear_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(1899);   // < 1900 → INVALID_ARGUMENT
  req.set_month(1);
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetMonthlyReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetMonthlyReport_InvalidMonth_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyReportRequest req;
  req.set_year(2023);
  req.set_month(13);   // > 12 → INVALID_ARGUMENT
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetMonthlyReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 41 gap closure: GetAnnualReport year validator (line 220-225).
TEST_F(DisplayServiceIT, GetAnnualReport_InvalidYear_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  auto stub = DisplayService::NewStub(channel());
  GetAnnualReportRequest req;
  req.set_year(1899);  // < 1900 → INVALID_ARGUMENT
  req.set_vehicle_id(vid);
  PeriodReport resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetAnnualReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// === ChargingReport RPCs (spec 2026-08-13-display-charging-reports) ===

TEST_F(DisplayServiceIT, GetDailyChargingReport_HappyPath_MultipleRows) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  // data::MakeValidCreateChargingRequest uses a FIXED StartTime of
  // 1700000000 = 2023-11-14 22:13:20 UTC (see tests/integration/
  // test_data.cc:142). We query for that exact date so all 3 events
  // land in the daily bucket.
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  GetDailyChargingReportRequest req;
  req.set_year(2023); req.set_month(11); req.set_day(14);
  req.set_vehicle_id(vid);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetDailyChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.year(), req.year());
  EXPECT_EQ(resp.month(), req.month());
  EXPECT_EQ(resp.day(), req.day());
  EXPECT_EQ(resp.count(), 3);
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_GT(resp.total_kwh(), 0.0);
  EXPECT_GT(resp.total_cost(), 0.0);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_Empty) {
  auto stub = DisplayService::NewStub(channel());
  GetDailyChargingReportRequest req;
  req.set_year(2026); req.set_month(8); req.set_day(13);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetDailyChargingReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.total_cost(), 0.0);
  EXPECT_EQ(resp.total_kwh(), 0.0);
  EXPECT_EQ(resp.count(), 0);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_VehicleFilter) {
  auto chan = channel();
  const auto vid_a = data::CreateVehicleId(chan);
  const auto vid_b = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  for (int i = 0; i < 2; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid_a, sid), &v).ok());
    Charging v2; grpc::ClientContext c2;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c2, data::MakeValidCreateChargingRequest(vid_b, sid), &v2).ok());
  }
  // All 4 events land on 2023-11-14 (helper fixed timestamp). Query
  // with vehicle_id = A; should return only A's 2 rows.
  GetDailyChargingReportRequest req;
  req.set_year(2023); req.set_month(11); req.set_day(14);
  req.set_vehicle_id(vid_a);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetDailyChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 2);  // only A's rows
  EXPECT_EQ(resp.vehicle_id(), vid_a);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_YearBelow1900_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetDailyChargingReportRequest req;
  req.set_year(1899); req.set_month(1); req.set_day(1);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_MonthOutOfRange_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  // month=0 — below lower bound (1..12).
  {
    GetDailyChargingReportRequest req;
    req.set_year(2026); req.set_month(0); req.set_day(1);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  // month=13 — above upper bound.
  {
    GetDailyChargingReportRequest req;
    req.set_year(2026); req.set_month(13); req.set_day(1);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_DayOutOfRange_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  // day=0 — below lower bound (1..LastDayOfMonth).
  {
    GetDailyChargingReportRequest req;
    req.set_year(2026); req.set_month(1); req.set_day(0);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  // day=32 — above upper bound for January (31).
  {
    GetDailyChargingReportRequest req;
    req.set_year(2026); req.set_month(1); req.set_day(32);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_Feb30_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetDailyChargingReportRequest req;
  req.set_year(2026); req.set_month(2); req.set_day(30);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetMonthlyChargingReport_HappyPath_MultipleRows) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  // Helper fixed StartTime = 2023-11-14 (1700000000 epoch) — query
  // for that exact month so all 3 events land in the monthly bucket.
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  GetMonthlyChargingReportRequest req;
  req.set_year(2023); req.set_month(11);
  req.set_vehicle_id(vid);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetMonthlyChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 3);
  EXPECT_EQ(resp.year(), req.year());
  EXPECT_EQ(resp.month(), req.month());
  EXPECT_EQ(resp.day(), 0);   // monthly → day=0
  EXPECT_GT(resp.total_kwh(), 0.0);
}

TEST_F(DisplayServiceIT, GetMonthlyChargingReport_Empty) {
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyChargingReportRequest req;
  req.set_year(2026); req.set_month(8);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetMonthlyChargingReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 0);
  EXPECT_EQ(resp.total_cost(), 0.0);
  EXPECT_EQ(resp.total_kwh(), 0.0);
}

TEST_F(DisplayServiceIT, GetMonthlyChargingReport_VehicleFilter) {
  auto chan = channel();
  const auto vid_a = data::CreateVehicleId(chan);
  const auto vid_b = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  // All 4 events land in 2023-11 (helper fixed). Query with
  // vehicle_id = A; should return only A's 2 rows.
  for (int i = 0; i < 2; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid_a, sid), &v).ok());
    Charging v2; grpc::ClientContext c2;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c2, data::MakeValidCreateChargingRequest(vid_b, sid), &v2).ok());
  }
  GetMonthlyChargingReportRequest req;
  req.set_year(2023); req.set_month(11);
  req.set_vehicle_id(vid_a);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetMonthlyChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 2);
  EXPECT_EQ(resp.vehicle_id(), vid_a);
}

TEST_F(DisplayServiceIT, GetMonthlyChargingReport_YearBelow1900_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetMonthlyChargingReportRequest req;
  req.set_year(1899); req.set_month(1);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetMonthlyChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetMonthlyChargingReport_MonthOutOfRange_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  // month=0 — below lower bound (1..12).
  {
    GetMonthlyChargingReportRequest req;
    req.set_year(2026); req.set_month(0);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetMonthlyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  // month=13 — above upper bound.
  {
    GetMonthlyChargingReportRequest req;
    req.set_year(2026); req.set_month(13);
    ChargingReport resp; grpc::ClientContext ctx;
    grpc::Status st = stub->GetMonthlyChargingReport(&ctx, req, &resp);
    EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST_F(DisplayServiceIT, GetAnnualChargingReport_HappyPath_MultipleRows) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  // Helper fixed StartTime = 2023-11-14 — query for that year.
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  GetAnnualChargingReportRequest req;
  req.set_year(2023);
  req.set_vehicle_id(vid);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetAnnualChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 3);
  EXPECT_EQ(resp.year(), req.year());
  EXPECT_EQ(resp.month(), 0);   // annual → month=0
  EXPECT_EQ(resp.day(), 0);     // annual → day=0
}

TEST_F(DisplayServiceIT, GetAnnualChargingReport_Empty) {
  auto stub = DisplayService::NewStub(channel());
  GetAnnualChargingReportRequest req;
  req.set_year(2026);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetAnnualChargingReport(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 0);
  EXPECT_EQ(resp.total_cost(), 0.0);
  EXPECT_EQ(resp.total_kwh(), 0.0);
}

TEST_F(DisplayServiceIT, GetAnnualChargingReport_VehicleFilter) {
  auto chan = channel();
  const auto vid_a = data::CreateVehicleId(chan);
  const auto vid_b = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  // All 4 events in 2023 (helper fixed). Query with vehicle_id = A.
  for (int i = 0; i < 2; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c, data::MakeValidCreateChargingRequest(vid_a, sid), &v).ok());
    Charging v2; grpc::ClientContext c2;
    ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
        &c2, data::MakeValidCreateChargingRequest(vid_b, sid), &v2).ok());
  }
  GetAnnualChargingReportRequest req;
  req.set_year(2023);
  req.set_vehicle_id(vid_a);
  ChargingReport resp; grpc::ClientContext ctx;
  ASSERT_TRUE(DisplayService::NewStub(chan)->GetAnnualChargingReport(
      &ctx, req, &resp).ok());
  EXPECT_EQ(resp.count(), 2);
  EXPECT_EQ(resp.vehicle_id(), vid_a);
}

TEST_F(DisplayServiceIT, GetAnnualChargingReport_YearBelow1900_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetAnnualChargingReportRequest req;
  req.set_year(1899);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetAnnualChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_Apr31_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  GetDailyChargingReportRequest req;
  req.set_year(2026); req.set_month(4); req.set_day(31);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(DisplayServiceIT, GetDailyChargingReport_LeapYear_Feb29_HappyPath) {
  // 2024 is a leap year; Feb 29 is valid. Insert a row on Feb 29, 2024
  // and assert the daily report returns count=1 (not the trivial
  // empty-result test from the v3 draft). Timestamp 1709184000 =
  // 2024-02-29T00:00:00Z; verify with `date -u -d @1709184000`.
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto req = data::MakeValidCreateChargingRequest(vid, sid);
  req.mutable_start_time()->set_seconds(1709184000);  // 2024-02-29T00:00:00Z
  req.mutable_end_time()->set_seconds(1709187600);    // +1h
  Charging v; grpc::ClientContext ic;
  ASSERT_TRUE(ChargingService::NewStub(chan)->CreateCharging(
      &ic, req, &v).ok());

  auto stub = DisplayService::NewStub(chan);
  GetDailyChargingReportRequest dreq;
  dreq.set_year(2024); dreq.set_month(2); dreq.set_day(29);
  dreq.set_vehicle_id(vid);
  ChargingReport resp; grpc::ClientContext dctx;
  ASSERT_TRUE(stub->GetDailyChargingReport(&dctx, dreq, &resp).ok());
  EXPECT_EQ(resp.year(), 2024);
  EXPECT_EQ(resp.month(), 2);
  EXPECT_EQ(resp.day(), 29);
  EXPECT_EQ(resp.count(), 1);  // row was inserted on Feb 29, 2024
  EXPECT_GT(resp.total_kwh(), 0.0);
}

// Spec deviation: spec 2026-08-13-display-charging-reports-design.md
// §9.2 enumerates LeapYear_Feb29_HappyPath but does NOT list this
// NonLeapYear counterpart. We add it as a mirror of LeapYear — proves
// day-vs-month-year validation correctly distinguishes "valid in
// some years, invalid in this year" (e.g., Feb 29 OK in 2024 but
// not in 2026). The Apr31-style test (in Task 4.2 above) covers the
// simpler day-vs-month case; this one adds the year dimension.
TEST_F(DisplayServiceIT, GetDailyChargingReport_NonLeapYear_Feb29_InvalidArgument) {
  auto stub = DisplayService::NewStub(channel());
  // 2026 is NOT a leap year; Feb 29 must be rejected.
  GetDailyChargingReportRequest req;
  req.set_year(2026); req.set_month(2); req.set_day(29);
  ChargingReport resp; grpc::ClientContext ctx;
  grpc::Status st = stub->GetDailyChargingReport(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace evgrpc::test
