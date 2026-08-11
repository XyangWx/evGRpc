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

}  // namespace evgrpc::test