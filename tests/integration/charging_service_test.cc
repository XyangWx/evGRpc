// Integration tests for ChargingService.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on service-shape behavior
// without re-minting bearer tokens or repeating fixture setup.
//
// Helpers (CreateVehicleId, CreateSourceCategoryId, CreateChargingId,
// MakeValidCreateChargingRequest, ToUpdateChargingRequest) live in
// evgrpc::test::data; see test_data.h for the contract.
//
// This file currently holds only the Task 15 smoke test
// (DataHelpers_ProduceValidIds) so that Chunk 3 Task 15 can land
// independently of the per-RPC tests in Tasks 16-19. The per-RPC
// TEST_F's (CreateCharging happy / duplicate-FK / invalid-arg,
// GetCharging happy / not-found, etc.) will be appended in their
// respective follow-up tasks.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/charging.grpc.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` accessor. Declared at the top of this file (not in
// service_test_fixtures.h) because each service's tests live in
// their own .cc — keeps the per-service fixtures local and lets
// the per-chunk plan add fields without bloating the shared header.
// Mirrors the VehicleServiceIT pattern established in Chunk 2.
class ChargingServiceIT : public ServiceITBase {};

// Task 15 smoke test: exercises every ChargingService-related helper
// in evgrpc::test::data (CreateVehicleId, CreateSourceCategoryId,
// CreateChargingId, MakeValidCreateChargingRequest) by chaining
// them together — vehicle → source_category → charging — and
// asserting each id round-trips as a non-empty 36-char UUID.
//
// Lives here (not in test_data.cc) so the smoke runs inside a
// ServiceITBase fixture and has access to channel(). The helpers
// are first consumed in this file, so a regression in any helper
// surfaces in the same compile unit as its first consumer — same
// pattern as Chunk 2's DataHelpers_MakeValidVehicleIsValid in
// vehicle_service_test.cc (Task 8).
TEST_F(ChargingServiceIT, DataHelpers_ProduceValidIds) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  EXPECT_FALSE(vid.empty());
  EXPECT_EQ(vid.size(), 36u);
  EXPECT_FALSE(sid.empty());
  EXPECT_EQ(sid.size(), 36u);
  const auto cid = data::CreateChargingId(chan, vid, sid);
  EXPECT_FALSE(cid.empty());
  EXPECT_EQ(cid.size(), 36u);
}

// Task 16: CreateCharging happy path. Exercises the full helper
// chain — vehicle → source_category → CreateCharging — and asserts
// the server-set id is non-empty and the FK refs round-trip
// unchanged in the response.
TEST_F(ChargingServiceIT, CreateCharging_HappyPath) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto stub = ChargingService::NewStub(chan);
  const auto req = data::MakeValidCreateChargingRequest(vid, sid);
  Charging resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateCharging(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.vehicle_id(), vid);
  EXPECT_EQ(resp.source_category_id(), sid);
}

// Task 16: CreateCharging with a non-existent vehicle FK. The
// production handler is expected to surface this as INVALID_ARGUMENT
// (the validator / FK pre-check rejects before the DB round-trip).
TEST_F(ChargingServiceIT, CreateCharging_InvalidVehicleId_InvalidArgument) {
  auto chan = channel();
  const auto sid = data::CreateSourceCategoryId(chan);  // real
  auto stub = ChargingService::NewStub(chan);
  auto req = data::MakeValidCreateChargingRequest(
      "00000000-0000-0000-0000-000000000000",  // non-existent vehicle
      sid);
  Charging resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 16: CreateCharging with kwh_charged=0.0. Production
// ValidateCharging rejects this as INVALID_ARGUMENT before any DB
// I/O — so the test asserts the validation surface, not the FK
// surface.
TEST_F(ChargingServiceIT, CreateCharging_NonPositiveKwh_InvalidArgument) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto stub = ChargingService::NewStub(chan);
  auto req = data::MakeValidCreateChargingRequest(vid, sid);
  req.set_kwh_charged(0.0);  // validator catches before DB
  Charging resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 17: GetCharging happy path. Exercises the full helper chain
// (vehicle → source_category → CreateChargingId) to mint a real id,
// then GetCharging and asserts the server returns the same id
// (proving the id round-trips through the DB and the gRPC layer).
TEST_F(ChargingServiceIT, GetCharging_HappyPath) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  const auto cid = data::CreateChargingId(chan, vid, sid);
  auto stub = ChargingService::NewStub(chan);
  GetChargingRequest greq;
  greq.set_id(cid);
  Charging got;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->GetCharging(&ctx, greq, &got).ok());
  EXPECT_EQ(got.id(), cid);
}

// Task 17: GetCharging with an id that does not exist (all-zero UUID).
// Production GetCharging is expected to surface this as NOT_FOUND
// (the SQL SELECT returns 0 rows). The all-zero UUID is a valid
// UUIDv4 shape so it parses cleanly; it's the canonical "not found"
// sentinel used elsewhere in the suite.
TEST_F(ChargingServiceIT, GetCharging_NotFound) {
  auto stub = ChargingService::NewStub(channel());
  GetChargingRequest req;
  req.set_id("00000000-0000-0000-0000-000000000000");
  Charging got;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetCharging(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

}  // namespace evgrpc::test