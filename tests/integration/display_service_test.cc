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

// The closing namespace brace lives at the END of the file.
// Tasks 32-39 will insert new TEST_Fs BEFORE this closing brace.
// Use edit-tool with oldText including the closing brace as anchor
// for appending subsequent TEST_Fs.

}  // namespace evgrpc::test
