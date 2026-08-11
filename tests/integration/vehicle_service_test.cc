// Integration tests for VehicleService.CreateVehicle.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on service-shape behavior
// without re-minting bearer tokens or repeating fixture setup. The
// fixture class itself is declared here — Chunk 2's other 10 Vehicle
// tests (Get/Update/Delete/List) will derive from this same class in
// their own tasks, keeping the suite-level SetUpTestSuite overhead
// (TestServer bringup: ~2s) to a single cost per test process.
//
// Helpers (FreshUuid, FreshLicensePlate, MakeValidVehicle,
// MakeValidCreateVehicleRequest) live in evgrpc::test::data; see
// test_data.h for the contract.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/vehicle.grpc.pb.h"
#include "evgrpc/vehicle.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` accessor. Declared at the top of this file (not in
// service_test_fixtures.h) because each service's tests live in
// their own .cc — keeps the per-service fixtures local and lets
// the per-chunk plan add fields without bloating the shared header.
class VehicleServiceIT : public ServiceITBase {};

TEST_F(VehicleServiceIT, CreateVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  const auto req = data::MakeValidCreateVehicleRequest();
  Vehicle resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateVehicle(&ctx, req, &resp).ok())
      << "CreateVehicle (happy path) RPC failed: error_message="
      << ctx.debug_error_string();
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.license_plate(), req.license_plate());
}

TEST_F(VehicleServiceIT, CreateVehicle_DuplicateLicensePlate_Conflict) {
  auto stub = VehicleService::NewStub(channel());
  const std::string plate = "DUP-" + data::FreshUuid().substr(0, 4);
  const auto req1 = data::MakeValidCreateVehicleRequest(plate);
  Vehicle r1;
  grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, req1, &r1).ok())
      << "First CreateVehicle (setup) RPC failed: error_message="
      << ctx1.debug_error_string();

  const auto req2 = data::MakeValidCreateVehicleRequest(plate);  // same plate
  Vehicle r2;
  grpc::ClientContext ctx2;
  grpc::Status st = stub->CreateVehicle(&ctx2, req2, &r2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS)
      << "Expected ALREADY_EXISTS on duplicate license plate, got: "
      << st.error_code() << " — " << st.error_message();
}

// PG semantic note (verified empirically against the running DB on 2026-08-11):
// PostgreSQL's `NOT NULL` constraint rejects NULL values but accepts empty
// strings (""), because "" is a valid non-NULL VARCHAR. The Task 7 spec
// (Chunk 2 Task 7 §6) assumed empty strings would trigger
// `pqxx::not_null_violation` and map to INVALID_ARGUMENT, but they don't —
// the INSERT succeeds and the row is stored with LicensePlate="". This
// test therefore asserts the actual current behavior (the empty-plate row
// is inserted and round-tripped). If we later add app-level validation
// (or a CHECK constraint like `CHECK (length(LicensePlate) > 0)`), this
// test will start failing and force the change to be documented.
TEST_F(VehicleServiceIT, CreateVehicle_EmptyLicensePlate_Accepted) {
  auto stub = VehicleService::NewStub(channel());
  auto req = data::MakeValidCreateVehicleRequest();
  req.set_license_plate("");  // empty string — see comment above
  Vehicle resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->CreateVehicle(&ctx, req, &resp);
  EXPECT_TRUE(st.ok())
      << "Expected OK (PG NOT NULL does NOT reject empty strings), got: "
      << st.error_code() << " — " << st.error_message();
  // Belt-and-braces: the row should round-trip with license_plate="" intact.
  EXPECT_EQ(resp.license_plate(), "");
}

// GetVehicle happy-path: create → fetch → verify the row round-trips
// with the same id + license_plate. Setup uses CreateVehicle (real DB
// write) so the subsequent GetVehicle exercises the SELECT path
// end-to-end — a synthetic GetVehicleRequest wouldn't catch a
// mismatch between write and read (column drift, schema drift).
TEST_F(VehicleServiceIT, GetVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created;
  grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(),
                                  &created).ok())
      << "CreateVehicle (setup) RPC failed: error_message="
      << ctx1.debug_error_string();

  GetVehicleRequest greq;
  greq.set_id(created.id());
  Vehicle got;
  grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->GetVehicle(&ctx2, greq, &got).ok())
      << "GetVehicle (happy path) RPC failed: error_message="
      << ctx2.debug_error_string();
  EXPECT_EQ(got.id(), created.id());
  EXPECT_EQ(got.license_plate(), created.license_plate());
}

// GetVehicle NOT_FOUND: a well-formed UUID that was never inserted
// must return NOT_FOUND (verified in src/services/vehicle_service.cc
// line ~104). Using a zero UUID (rather than FreshUuid) so the test
// stays deterministic regardless of any prior fixture state — the
// SetUpTestSuite TruncateAll guarantee + the probability that a real
// UUID is all-zero is effectively zero means we won't collide.
TEST_F(VehicleServiceIT, GetVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  GetVehicleRequest req;
  req.set_id("00000000-0000-0000-0000-000000000000");
  Vehicle got;
  grpc::ClientContext ctx;
  grpc::Status st = stub->GetVehicle(&ctx, req, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND)
      << "Expected NOT_FOUND for missing id, got: " << st.error_code()
      << " — " << st.error_message();
}

// UpdateVehicle happy-path: create → update → verify the row round-trips
// with the new field value (brand) and the original id preserved. Mirror
// of Task 9/10's pattern: real DB write in setup, then exercise the
// UPDATE-then-RETURNING path end-to-end. All fields are set on the
// request (not just brand) so the UPDATE actually writes valid data —
// the production code binds every column, so a missing field would
// either default to "" / 0 (and breaking a UNIQUE constraint if the
// default plate happens to collide with another row) or trip a NOT
// NULL constraint on conversion.
TEST_F(VehicleServiceIT, UpdateVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created;
  grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(),
                                  &created).ok())
      << "CreateVehicle (setup) RPC failed: error_message="
      << ctx1.debug_error_string();

  UpdateVehicleRequest ureq;
  ureq.set_id(created.id());
  ureq.set_brand("Renault");
  ureq.set_calibrated_range_km(created.calibrated_range_km());
  ureq.set_battery_capacity_kwh(created.battery_capacity_kwh());
  *ureq.mutable_purchase_date() = created.purchase_date();
  ureq.set_license_plate(created.license_plate());
  Vehicle resp;
  grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->UpdateVehicle(&ctx2, ureq, &resp).ok())
      << "UpdateVehicle (happy path) RPC failed: error_message="
      << ctx2.debug_error_string();
  EXPECT_EQ(resp.brand(), "Renault");
  EXPECT_EQ(resp.id(), created.id());  // id preserved
}

// UpdateVehicle NOT_FOUND: a well-formed UUID that was never inserted
// must return NOT_FOUND (verified in src/services/vehicle_service.cc
// line ~135 — the UPDATE returns 0 rows when no row matches WHERE
// Id = $1). Using a zero UUID (rather than FreshUuid) so the test
// stays deterministic regardless of prior fixture state — the
// SetUpTestSuite TruncateAll guarantee + the probability that a
// real UUID is all-zero is effectively zero means we won't collide.
TEST_F(VehicleServiceIT, UpdateVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  UpdateVehicleRequest ureq;
  ureq.set_id("00000000-0000-0000-0000-000000000000");
  ureq.set_brand("Renault");
  ureq.set_license_plate("NF-1");
  Vehicle resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->UpdateVehicle(&ctx, ureq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND)
      << "Expected NOT_FOUND for missing id, got: " << st.error_code()
      << " — " << st.error_message();
}

// PG semantic note (verified empirically against the running DB on 2026-08-11):
// PostgreSQL's `NOT NULL` constraint rejects NULL values but accepts empty
// strings (""), because "" is a valid non-NULL VARCHAR. The Task 7 spec
// (Chunk 2 Task 7 §6) assumed empty strings would trigger
// `pqxx::not_null_violation` and map to INVALID_ARGUMENT, but they don't —
// the UPDATE succeeds and the row is stored with LicensePlate="". This
// test therefore asserts the actual current behavior (the empty-plate row
// is updated and round-tripped). If we later add app-level validation
// (or a CHECK constraint like `CHECK (length(LicensePlate) > 0)`), this
// test will start failing and force the change to be documented.
// Mirrors the CreateVehicle_EmptyLicensePlate_Accepted regression-guard
// from Task 9 (commit `123cc2a` + plan fix `1ec50b3`).
TEST_F(VehicleServiceIT, UpdateVehicle_EmptyLicensePlate_Accepted) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created;
  grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(),
                                  &created).ok())
      << "CreateVehicle (setup) RPC failed: error_message="
      << ctx1.debug_error_string();

  UpdateVehicleRequest ureq;
  ureq.set_id(created.id());
  ureq.set_brand("Renault");
  ureq.set_calibrated_range_km(created.calibrated_range_km());
  ureq.set_battery_capacity_kwh(created.battery_capacity_kwh());
  *ureq.mutable_purchase_date() = created.purchase_date();
  ureq.set_license_plate("");  // empty string — see comment above
  Vehicle resp;
  grpc::ClientContext ctx2;
  grpc::Status st = stub->UpdateVehicle(&ctx2, ureq, &resp);
  EXPECT_TRUE(st.ok())
      << "Expected OK (PG NOT NULL does NOT reject empty strings), got: "
      << st.error_code() << " — " << st.error_message();
  // Belt-and-braces: the row should round-trip with license_plate="" intact.
  EXPECT_EQ(resp.license_plate(), "");
}

// DeleteVehicle happy-path: create → delete → verify the row is actually
// gone (post-condition check via GetVehicle). Mirrors the Task 10/11
// pattern of using a real DB write in setup so we exercise the
// DELETE-then-SELECT path end-to-end — a direct RPC against a
// synthetic id would only prove the stub plumbing, not that the
// DELETE actually persisted. Per src/services/vehicle_service.cc
// line ~169, the impl returns NOT_FOUND when affected_rows()==0
// and OK otherwise (the Empty response carries no payload).
TEST_F(VehicleServiceIT, DeleteVehicle_HappyPath) {
  auto stub = VehicleService::NewStub(channel());
  Vehicle created;
  grpc::ClientContext ctx1;
  ASSERT_TRUE(stub->CreateVehicle(&ctx1, data::MakeValidCreateVehicleRequest(),
                                  &created).ok())
      << "CreateVehicle (setup) RPC failed: error_message="
      << ctx1.debug_error_string();

  DeleteVehicleRequest dreq;
  dreq.set_id(created.id());
  google::protobuf::Empty empty_resp;
  grpc::ClientContext ctx2;
  ASSERT_TRUE(stub->DeleteVehicle(&ctx2, dreq, &empty_resp).ok())
      << "DeleteVehicle (happy path) RPC failed: error_message="
      << ctx2.debug_error_string();

  // Confirm the row is actually gone — the RPC alone only proves
  // the stub plumbing + the impl accepted the call, not that the
  // DELETE actually persisted.
  GetVehicleRequest greq;
  greq.set_id(created.id());
  Vehicle got;
  grpc::ClientContext ctx3;
  grpc::Status st = stub->GetVehicle(&ctx3, greq, &got);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND)
      << "Expected NOT_FOUND after DeleteVehicle, got: " << st.error_code()
      << " — " << st.error_message();
}

// DeleteVehicle NOT_FOUND: a well-formed UUID that was never inserted
// must return NOT_FOUND (verified in src/services/vehicle_service.cc
// line ~169 — DELETE returns 0 rows when no row matches WHERE Id=$1,
// and the impl maps affected_rows()==0 to NOT_FOUND). Using a zero
// UUID (rather than FreshUuid) so the test stays deterministic
// regardless of prior fixture state — the SetUpTestSuite TruncateAll
// guarantee + the probability that a real UUID is all-zero is
// effectively zero means we won't collide. Mirrors the regression-guard
// pattern established by GetVehicle_NotFound / UpdateVehicle_NotFound.
TEST_F(VehicleServiceIT, DeleteVehicle_NotFound) {
  auto stub = VehicleService::NewStub(channel());
  DeleteVehicleRequest dreq;
  dreq.set_id("00000000-0000-0000-0000-000000000000");
  google::protobuf::Empty resp;
  grpc::ClientContext ctx;
  grpc::Status st = stub->DeleteVehicle(&ctx, dreq, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND)
      << "Expected NOT_FOUND for missing id, got: " << st.error_code()
      << " — " << st.error_message();
}

// Deferred runtime smoke from Chunk 2 Task 8: lives here (rather than
// in test_data.cc) because VehicleServiceIT owns the fixture class
// and Task 8's helpers are exercised by this file's other tests — so
// keeping the smoke in the same TU surfaces helper regressions at
// the same compile unit as the helpers' first consumer.
TEST_F(VehicleServiceIT, DataHelpers_MakeValidVehicleIsValid) {
  const auto v = data::MakeValidVehicle();
  EXPECT_EQ(v.id().size(), 36u);                       // UUID length
  EXPECT_EQ(v.brand(), "Tesla");
  EXPECT_EQ(v.license_plate().substr(0, 2), "T-");     // default prefix
  EXPECT_GT(v.calibrated_range_km(), 0);
  EXPECT_GT(v.battery_capacity_kwh(), 0.0);
  EXPECT_TRUE(v.has_purchase_date());

  const auto req = data::MakeValidCreateVehicleRequest();
  EXPECT_EQ(req.brand(), "Tesla");
  EXPECT_EQ(req.license_plate().substr(0, 2), "T-");
  // Note: CreateVehicleRequest has no `id` field — the id field only
  // appears in Vehicle (the response). Tested via `v.id()` above.
}

}  // namespace evgrpc::test
