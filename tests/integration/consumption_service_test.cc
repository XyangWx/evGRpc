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

}  // namespace evgrpc::test