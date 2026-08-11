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

// The closing namespace brace lives at the END of the file.
// Tasks 32-39 will insert new TEST_Fs BEFORE this closing brace.
// Use edit-tool with oldText including the closing brace as anchor
// for appending subsequent TEST_Fs.

}  // namespace evgrpc::test
