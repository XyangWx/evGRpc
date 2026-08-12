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

// Task 18: UpdateCharging happy path. Creates a charging via the
// helper chain, then issues an Update that overrides kwh_charged
// (50.0 -> 60.0). The template comes from a fresh
// MakeValidCreateChargingRequest (so all validator-required fields
// stay valid), converted to UpdateChargingRequest via the
// ToUpdateChargingRequest helper. Asserts the response echoes the
// new kwh and the same id as the create — proves the SQL UPDATE
// found the row, applied the change, and round-tripped through
// the gRPC layer.
TEST_F(ChargingServiceIT, UpdateCharging_HappyPath) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto stub = ChargingService::NewStub(chan);
  // Create first to get a valid id
  Charging created;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateCharging(
      &c1, data::MakeValidCreateChargingRequest(vid, sid), &created).ok());
  // Build update request from a fresh valid template + override kwh
  auto template_req = data::MakeValidCreateChargingRequest(vid, sid);
  template_req.set_kwh_charged(60.0);  // the change
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(template_req);
  ureq.set_id(created.id());
  Charging resp;
  grpc::ClientContext c2;
  ASSERT_TRUE(stub->UpdateCharging(&c2, ureq, &resp).ok());
  EXPECT_EQ(resp.kwh_charged(), 60.0);
  EXPECT_EQ(resp.id(), created.id());
}

// Task 18: UpdateCharging with an id that does not exist (all-zero
// UUID). The template is fully-valid (from MakeValidCreateChargingRequest +
// ToUpdateChargingRequest), so ValidateCharging passes; the SQL UPDATE
// finds 0 rows and production surfaces NOT_FOUND. Confirms the
// handler distinguishes "bad input" (INVALID_ARGUMENT) from "target
// row missing" (NOT_FOUND) — the validator runs first, then DB.
TEST_F(ChargingServiceIT, UpdateCharging_NotFound) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto stub = ChargingService::NewStub(chan);
  // Build a fully-valid update request but with a non-existent id.
  // The validator (re-run inside UpdateCharging) passes because all
  // template fields are valid; the SQL UPDATE finds 0 rows → NOT_FOUND.
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(
      data::MakeValidCreateChargingRequest(vid, sid));
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  Charging resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateCharging(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// Task 18: UpdateCharging with end_time == start_time. ValidateCharging
// requires end_time > start_time; setting them equal (proto3 mutable_X
// pattern returns a Timestamp* that we can assign the start_time
// Timestamp value into) makes the validator reject the request as
// INVALID_ARGUMENT before any DB I/O. Same surface as the
// CreateCharging validator, but exercised via the Update path so the
// validator's reuse on Update is regression-guarded.
TEST_F(ChargingServiceIT, UpdateCharging_EndTimeBeforeStart_InvalidArgument) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  auto stub = ChargingService::NewStub(chan);
  Charging created;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateCharging(
      &c1, data::MakeValidCreateChargingRequest(vid, sid), &created).ok());
  // Build update request where end_time == start_time (validator rejects)
  auto template_req = data::MakeValidCreateChargingRequest(vid, sid);
  *template_req.mutable_end_time() = template_req.start_time();
  UpdateChargingRequest ureq = data::ToUpdateChargingRequest(template_req);
  ureq.set_id(created.id());
  Charging resp;
  grpc::ClientContext c2;
  grpc::Status st = stub->UpdateCharging(&c2, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Gap closure: setting service_fee exercises the nullable wrapper
// append path (line 123) and the read-back parse (line 54).
TEST_F(ChargingServiceIT, CreateCharging_WithServiceFee_RoundTrip) {
  const auto vid = data::CreateVehicleId(channel());
  const auto sid = data::CreateSourceCategoryId(channel());
  auto stub = ChargingService::NewStub(channel());
  CreateChargingRequest req = data::MakeValidCreateChargingRequest(vid, sid);
  req.mutable_service_fee()->set_value(2.50);
  Charging resp;
  grpc::ClientContext c;
  ASSERT_TRUE(stub->CreateCharging(&c, req, &resp).ok());
  EXPECT_TRUE(resp.has_service_fee());
  EXPECT_DOUBLE_EQ(resp.service_fee().value(), 2.50);
}

// Gap closure: setting remark exercises the non-empty append path
// (line 139) and the read-back (line 62).
TEST_F(ChargingServiceIT, CreateCharging_WithRemark_RoundTrip) {
  const auto vid = data::CreateVehicleId(channel());
  const auto sid = data::CreateSourceCategoryId(channel());
  auto stub = ChargingService::NewStub(channel());
  CreateChargingRequest req = data::MakeValidCreateChargingRequest(vid, sid);
  req.set_remark("test charging");
  Charging resp;
  grpc::ClientContext c;
  ASSERT_TRUE(stub->CreateCharging(&c, req, &resp).ok());
  EXPECT_EQ(resp.remark(), "test charging");
}

// Gap closure: empty location exercises the NULL append path (line 132).
TEST_F(ChargingServiceIT, CreateCharging_EmptyLocation_NullRoundTrip) {
  const auto vid = data::CreateVehicleId(channel());
  const auto sid = data::CreateSourceCategoryId(channel());
  auto stub = ChargingService::NewStub(channel());
  CreateChargingRequest req = data::MakeValidCreateChargingRequest(vid, sid);
  req.clear_location();
  Charging resp;
  grpc::ClientContext c;
  ASSERT_TRUE(stub->CreateCharging(&c, req, &resp).ok());
  EXPECT_TRUE(resp.location().empty());
}

// Gap closure: CreateCharging with cost <= 0 exercises the cost
// validator at charging_service.cc line 86-89.
TEST_F(ChargingServiceIT, CreateCharging_NonPositiveCost_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto sid = data::CreateSourceCategoryId(channel());
  auto stub = ChargingService::NewStub(channel());
  auto req = data::MakeValidCreateChargingRequest(vid, sid);
  req.set_cost(0.0);
  Charging resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Gap closure: CreateCharging with end_percent <= start_percent
// exercises the end_percent validator at line 78-81.
TEST_F(ChargingServiceIT, CreateCharging_NonIncreasingPercent_InvalidArgument) {
  const auto vid = data::CreateVehicleId(channel());
  const auto sid = data::CreateSourceCategoryId(channel());
  auto stub = ChargingService::NewStub(channel());
  auto req = data::MakeValidCreateChargingRequest(vid, sid);
  req.set_end_percent(20);
  Charging resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateCharging(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// Task 19: DeleteCharging happy path. Mints a charging via the helper
// chain, deletes it, then asserts the post-condition by issuing a
// GetCharging for the same id and expecting NOT_FOUND — proves the
// DELETE actually removed the row (not just no-op'd). The delete RPC
// returns google.protobuf.Empty per the proto definition, so we
// hand a google::protobuf::Empty to receive the (empty) response.
// Uses `chan` (not `channel`) to avoid shadowing ServiceITBase::channel().
TEST_F(ChargingServiceIT, DeleteCharging_HappyPath) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  const auto cid = data::CreateChargingId(chan, vid, sid);
  auto stub = ChargingService::NewStub(chan);
  DeleteChargingRequest dreq;
  dreq.set_id(cid);
  google::protobuf::Empty empty;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->DeleteCharging(&c1, dreq, &empty).ok());
  // Confirm gone
  GetChargingRequest greq;
  greq.set_id(cid);
  Charging got;
  grpc::ClientContext c2;
  EXPECT_EQ(stub->GetCharging(&c2, greq, &got).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

// Task 19: DeleteCharging with an id that does not exist (all-zero
// UUID). The delete is a no-op SQL-wise; production is expected to
// surface this as NOT_FOUND so callers can distinguish "already gone"
// from "delete failed". Same all-zero-UUID sentinel used by Get/Update
// NotFound cases for consistency.
TEST_F(ChargingServiceIT, DeleteCharging_NotFound) {
  auto stub = ChargingService::NewStub(channel());
  DeleteChargingRequest dreq;
  dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteCharging(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

// Task 20: ListChargings happy path. Mints 3 chargings via the helper
// chain (vehicle + source_category + 3x CreateCharging), then issues
// an unpaginated ListChargings and asserts all 3 rows come back with
// an empty next_page_token (the no-more-pages sentinel). Uses `chan`
// (not `channel`) to avoid shadowing ServiceITBase::channel().
TEST_F(ChargingServiceIT, ListChargings_HappyPath_MultipleRows) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  for (int i = 0; i < 3; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(
        ChargingService::NewStub(chan)->CreateCharging(
            &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  auto stub = ChargingService::NewStub(chan);
  ListChargingsRequest req;
  ListChargingsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListChargings(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.chargings_size(), 3);
  EXPECT_TRUE(resp.next_page_token().empty());
}

// Task 20: ListChargings empty case. TruncateAll (per-test fixture)
// has cleared all tables, so a default ListChargings request returns
// an empty result with an empty next_page_token (no rows, no more
// pages). Direct channel() call — no `chan` rename needed because
// nothing here would shadow.
TEST_F(ChargingServiceIT, ListChargings_Empty) {
  auto stub = ChargingService::NewStub(channel());
  ListChargingsRequest req;
  ListChargingsResponse resp; grpc::ClientContext ctx;
  ASSERT_TRUE(stub->ListChargings(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.chargings_size(), 0);
  EXPECT_TRUE(resp.next_page_token().empty());
}

// Task 20: ListChargings pagination. Mints 5 chargings, requests
// page_size=2 for the first page, asserts 2 rows come back with a
// non-empty next_page_token (the has_more branch — server signals
// there is more), then issues a second request with the token + same
// page_size=2 and asserts another 2 rows come back. Exercises the
// page-token encode/decode path through the gRPC layer. Uses `chan`
// (not `channel`) to avoid shadowing ServiceITBase::channel().
TEST_F(ChargingServiceIT, ListChargings_Pagination) {
  auto chan = channel();
  const auto vid = data::CreateVehicleId(chan);
  const auto sid = data::CreateSourceCategoryId(chan);
  for (int i = 0; i < 5; ++i) {
    Charging v; grpc::ClientContext c;
    ASSERT_TRUE(
        ChargingService::NewStub(chan)->CreateCharging(
            &c, data::MakeValidCreateChargingRequest(vid, sid), &v).ok());
  }
  auto stub = ChargingService::NewStub(chan);
  // First page: page_size=2 → 2 rows + non-empty next_page_token
  ListChargingsRequest req1; req1.set_page_size(2);
  ListChargingsResponse resp1; grpc::ClientContext c1;
  ASSERT_TRUE(stub->ListChargings(&c1, req1, &resp1).ok());
  EXPECT_EQ(resp1.chargings_size(), 2);
  EXPECT_FALSE(resp1.next_page_token().empty());
  // Second page with token
  ListChargingsRequest req2;
  req2.set_page_size(2);
  req2.set_page_token(resp1.next_page_token());
  ListChargingsResponse resp2; grpc::ClientContext c2;
  ASSERT_TRUE(stub->ListChargings(&c2, req2, &resp2).ok());
  EXPECT_EQ(resp2.chargings_size(), 2);
}

}  // namespace evgrpc::test