// Integration tests for ConsumptionService.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on service-shape behavior
// without re-minting bearer tokens or repeating fixture setup.
//
// Helpers (CreateVehicleId, CreateWeatherId, CreateConsumptionId,
// MakeValidCreateConsumptionRequest, ToUpdateConsumptionRequest)
// live in evgrpc::test::data; see test_data.h for the contract.
//
// This file currently holds only the Task 23 smoke test
// (DataHelpers_ProduceValidIds) so that Chunk 4 Task 23 can land
// independently of the per-RPC tests in Tasks 24-28. The per-RPC
// TEST_F's (CreateConsumption happy / invalid-vehicle / temp-validation,
// GetConsumption happy / not-found, etc.) will be appended in their
// respective follow-up tasks.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/consumption.grpc.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` / `pg()` accessors. Declared at the top of this file
// (not in service_test_fixtures.h) because each service's tests live
// in their own .cc — keeps the per-service fixtures local and lets
// the per-chunk plan add fields without bloating the shared header.
// Mirrors the ChargingServiceIT pattern established in Chunk 3.
class ConsumptionServiceIT : public ServiceITBase {};

// Task 23 smoke test: exercises every ConsumptionService-related helper
// in evgrpc::test::data (CreateVehicleId, CreateWeatherId,
// CreateConsumptionId, MakeValidCreateConsumptionRequest) by chaining
// them together — vehicle → weather (direct SQL) → consumption — and
// asserting each id round-trips as a non-empty 36-char UUID.
//
// Lives here (not in test_data.cc) so the smoke runs inside a
// ServiceITBase fixture and has access to channel() and pg(). The
// helpers are first consumed in this file, so a regression in any
// helper surfaces in the same compile unit as its first consumer —
// same pattern as Chunk 3's DataHelpers_ProduceValidIds in
// charging_service_test.cc (Task 15).
TEST_F(ConsumptionServiceIT, DataHelpers_ProduceValidIds) {
  const auto vid = data::CreateVehicleId(channel());
  EXPECT_FALSE(vid.empty());
  EXPECT_EQ(vid.size(), 36u);
  const auto wid = data::CreateWeatherId(pg());
  EXPECT_FALSE(wid.empty());
  EXPECT_EQ(wid.size(), 36u);
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  EXPECT_FALSE(cid.empty());
  EXPECT_EQ(cid.size(), 36u);
}

// Task 24: CreateConsumption happy path. Exercises the full helper
// chain — vehicle → weather (direct SQL) → CreateConsumption — and
// asserts the server-set id is non-empty and both FK refs round-trip
// unchanged in the response.
TEST_F(ConsumptionServiceIT, CreateConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  const auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  Consumption resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateConsumption(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_EQ(resp.weather_id(), wid);
}

// Task 24: CreateConsumption with a non-existent vehicle FK. The
// production handler is expected to surface this as INVALID_ARGUMENT
// (FK pre-check rejects before the DB round-trip — same path as
// Chunk 3's ChargingServiceIT.CreateCharging_InvalidVehicleId).
TEST_F(ConsumptionServiceIT, CreateConsumption_InvalidVehicleId_InvalidArgument) {
  const auto wid = data::CreateWeatherId(pg());  // real
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(
      "00000000-0000-0000-0000-000000000000",  // non-existent vehicle
      wid);
  Consumption resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateConsumption(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 24: CreateConsumption with highest_temperature_c <
// lowest_temperature_c. ValidateConsumption rejects before the DB
// round-trip (line 81-83 of consumption_service.cc).
TEST_F(ConsumptionServiceIT, CreateConsumption_HighestTempLtLowest_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  req.set_highest_temperature_c(5.0);
  req.set_lowest_temperature_c(20.0);  // highest < lowest -> INVALID_ARGUMENT
  Consumption resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateConsumption(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 25: GetConsumption happy path. Uses the auto-creating
// CreateConsumptionId helper (which also auto-creates the
// prerequisite weather row) so the test focuses on the GET RPC
// round-trip. Asserts both id and vehicle_id round-trip — weather_id
// isn't asserted because the helper hides that FK.
TEST_F(ConsumptionServiceIT, GetConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  auto stub = ConsumptionService::NewStub(channel());
  GetConsumptionRequest greq;
  greq.set_id(cid);
  Consumption got;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumption(&ctx, greq, &got).ok());
  EXPECT_EQ(got.id(), cid);
  EXPECT_EQ(got.vehicle_id(), vid);
}

// Task 25: GetConsumption with an all-zero UUID — exercises the
// NOT_FOUND branch (no row exists, server returns 0 rows from
// SELECT).
TEST_F(ConsumptionServiceIT, GetConsumption_NotFound) {
  auto stub = ConsumptionService::NewStub(channel());
  GetConsumptionRequest req;
  req.set_id("00000000-0000-0000-0000-000000000000");
  Consumption got;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetConsumption(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// Task 26: UpdateConsumption happy path. Creates a consumption row,
// then builds a fresh valid template via MakeValidCreateConsumptionRequest,
// overrides one field (end_mileage_km) to make the update meaningful,
// converts to UpdateConsumptionRequest via ToUpdateConsumptionRequest,
// sets id, and asserts the change round-trips in the response.
TEST_F(ConsumptionServiceIT, UpdateConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  Consumption created;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateConsumption(
      &c1, data::MakeValidCreateConsumptionRequest(vid, wid), &created)
                  .ok());
  auto template_req = data::MakeValidCreateConsumptionRequest(vid, wid);
  template_req.set_end_mileage_km(10200);  // the change
  UpdateConsumptionRequest ureq =
      data::ToUpdateConsumptionRequest(template_req);
  ureq.set_id(created.id());
  Consumption resp;
  grpc::ClientContext c2;
  ASSERT_TRUE(stub->UpdateConsumption(&c2, ureq, &resp).ok());
  EXPECT_EQ(resp.end_mileage_km(), 10200);
  EXPECT_EQ(resp.id(), created.id());
}

// Task 26: UpdateConsumption NOT_FOUND. Builds a fully-valid
// UpdateConsumptionRequest from a fresh valid template + all-zero
// UUID. The validator passes (template fields are valid), the SQL
// UPDATE finds 0 rows -> NOT_FOUND. Same pattern as Chunk 3's
// ChargingServiceIT.UpdateCharging_NotFound.
TEST_F(ConsumptionServiceIT, UpdateConsumption_NotFound) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  UpdateConsumptionRequest ureq = data::ToUpdateConsumptionRequest(
      data::MakeValidCreateConsumptionRequest(vid, wid));
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  Consumption resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateConsumption(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// Task 26: UpdateConsumption with invalid temp ordering. The
// production validator (re-run inside UpdateConsumption) rejects
// highest < lowest before the DB round-trip.
TEST_F(ConsumptionServiceIT, UpdateConsumption_TempValidation_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  Consumption created;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateConsumption(
      &c1, data::MakeValidCreateConsumptionRequest(vid, wid), &created)
                  .ok());
  auto template_req = data::MakeValidCreateConsumptionRequest(vid, wid);
  template_req.set_highest_temperature_c(0.0);
  template_req.set_lowest_temperature_c(20.0);
  UpdateConsumptionRequest ureq =
      data::ToUpdateConsumptionRequest(template_req);
  ureq.set_id(created.id());
  Consumption resp;
  grpc::ClientContext c2;
  grpc::Status st = stub->UpdateConsumption(&c2, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 27: DeleteConsumption happy path. Creates a consumption row
// via the auto-creating helper, deletes it, then re-fetches via
// GetConsumption to confirm the row is gone (post-condition check
// via the same channel). Uses google::protobuf::Empty for the
// Delete response (delete RPCs return Empty, not the deleted row).
TEST_F(ConsumptionServiceIT, DeleteConsumption_HappyPath) {
  const auto vid = data::CreateVehicleId(channel());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  auto stub = ConsumptionService::NewStub(channel());
  DeleteConsumptionRequest dreq;
  dreq.set_id(cid);
  google::protobuf::Empty empty;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->DeleteConsumption(&c1, dreq, &empty).ok());
  // Post-condition: the row is gone.
  GetConsumptionRequest greq;
  greq.set_id(cid);
  Consumption got;
  grpc::ClientContext c2;
  EXPECT_EQ(stub->GetConsumption(&c2, greq, &got).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

// Gap closure: round-trip read after create exercises the
// ParseTimestamp success path in RowToConsumption (lines 40, 46 of
// consumption_service.cc). Same fix as charging_service.cc ParseTimestamp
// (this run's production fix) — handles PG TIMESTAMP format with
// microseconds + tz offset.
TEST_F(ConsumptionServiceIT, CreateConsumption_GetConsumption_RoundTrip_Timestamps) {
  const auto vid = data::CreateVehicleId(channel());
  const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
  auto stub = ConsumptionService::NewStub(channel());
  GetConsumptionRequest greq;
  greq.set_id(cid);
  Consumption got;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetConsumption(&ctx, greq, &got).ok());
  EXPECT_EQ(got.start().seconds(), 1700000000);
  EXPECT_EQ(got.end().seconds(), 1700003600);
}

// Gap closure: round-trip read after create exercises the
// ParseTimestamp success path in RowToConsumption (lines 40, 46 of
// consumption_service.cc) plus the remark set path (line 64).
TEST_F(ConsumptionServiceIT, CreateConsumption_WithRemark_RoundTrip) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  req.set_remark("trip notes");
  Consumption resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateConsumption(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.remark(), "trip notes");
}

// Gap closure: end <= start exercises the end>start validator at
// consumption_service.cc line 75-78.
TEST_F(ConsumptionServiceIT, CreateConsumption_EndLeStart_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto wid = data::CreateWeatherId(pg());
  auto stub = ConsumptionService::NewStub(channel());
  auto req = data::MakeValidCreateConsumptionRequest(vid, wid);
  *req.mutable_end() = req.start();  // end == start -> INVALID_ARGUMENT
  Consumption resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateConsumption(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 27: DeleteConsumption with an all-zero UUID — exercises the
// NOT_FOUND branch (no row exists, server returns 0 rows from DELETE).
TEST_F(ConsumptionServiceIT, DeleteConsumption_NotFound) {
  auto stub = ConsumptionService::NewStub(channel());
  DeleteConsumptionRequest dreq;
  dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteConsumption(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// Task 28: ListConsumptions happy path with 3 rows (no pagination).
// Uses the auto-creating CreateConsumptionId helper in a loop — each
// iteration also auto-creates a weather row, so total rows in the
// consumption table = 3. Asserts no pagination token is returned
// when total rows <= default page_size.
TEST_F(ConsumptionServiceIT, ListConsumptions_HappyPath_MultipleRows) {
  const auto vid = data::CreateVehicleId(channel());
  for (int i = 0; i < 3; ++i) {
    const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
    EXPECT_FALSE(cid.empty());
  }
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req;
  ListConsumptionsResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListConsumptions(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.consumptions_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());
}

// Task 28: ListConsumptions empty case. No rows in the table (each
// test runs after TruncateAll in SetUp), so the response is empty
// and next_page_token is empty.
TEST_F(ConsumptionServiceIT, ListConsumptions_Empty) {
  auto stub = ConsumptionService::NewStub(channel());
  ListConsumptionsRequest req;
  ListConsumptionsResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListConsumptions(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.consumptions_size(), 0);
  EXPECT_TRUE(resp.next_page_token().empty());
}

// Task 28: ListConsumptions pagination — creates 5 rows, asks for
// page_size=2, asserts first page returns 2 rows + non-empty
// next_page_token (the `has_more` branch), then asks for the second
// page with the returned token and asserts another 2 rows come back.
// Same pattern as ChargingServiceIT.ListChargings_Pagination.
TEST_F(ConsumptionServiceIT, ListConsumptions_Pagination) {
  const auto vid = data::CreateVehicleId(channel());
  for (int i = 0; i < 5; ++i) {
    const auto cid = data::CreateConsumptionId(channel(), pg(), vid);
    EXPECT_FALSE(cid.empty());
  }
  auto stub = ConsumptionService::NewStub(channel());
  // First page: page_size=2 -> 2 rows + non-empty next_page_token.
  ListConsumptionsRequest req1;
  req1.set_page_size(2);
  ListConsumptionsResponse resp1;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->ListConsumptions(&c1, req1, &resp1).ok());
  EXPECT_EQ(resp1.consumptions_size(), 2);
  EXPECT_FALSE(resp1.next_page_token().empty());
  // Second page with token.
  ListConsumptionsRequest req2;
  req2.set_page_size(2);
  req2.set_page_token(resp1.next_page_token());
  ListConsumptionsResponse resp2;
  grpc::ClientContext c2;
  ASSERT_TRUE(stub->ListConsumptions(&c2, req2, &resp2).ok());
  EXPECT_EQ(resp2.consumptions_size(), 2);
}

}  // namespace evgrpc::test