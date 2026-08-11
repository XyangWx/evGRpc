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

}  // namespace evgrpc::test