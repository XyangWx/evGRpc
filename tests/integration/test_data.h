#pragma once

// Test-data helpers shared across the per-service integration suites
// (DisplayService today; ChargingService / ConsumptionService will
// extend in their own chunks).
//
// Two layers:
//   * Pure value constructors (no I/O, no gRPC) — used to build
//     strongly-typed proto messages with default-valid fields.
//   * Setup helpers that round-trip a request through the running
//     TestServer and return the server-side id, so tests don't have
//     to repeat the RPC boilerplate for every fixture setup.
//
// Helpers live in `evgrpc::test::data` (nested under the test
// namespace so the production namespace stays clean).

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>

#include "evgrpc/vehicle.pb.h"
#include "fixtures/pg_container.h"

namespace evgrpc::test::data {

// UUID helper — thin wrapper around evgrpc::NewUuid() so test bodies
// don't need to include util/uuid.h.
std::string FreshUuid();

// License plate generator (e.g. "T-1a2b3c4d"). Truncated UUID is
// unique enough for the per-test freshness contract (no two tests
// should generate the same plate; collisions across processes are
// astronomically unlikely).
std::string FreshLicensePlate();

// Build a fully-valid Vehicle proto for use as test fixtures /
// expected-shape comparisons (CreateVehicleRequest == Vehicle fields
// minus id, so this also works as CreateVehicleRequest input when
// callers don't need to assert "id was dropped").
Vehicle MakeValidVehicle(std::string plate = "");

// Build a CreateVehicleRequest — Vehicle fields minus the server-set
// `id`. Used as RPC input to `VehicleService.CreateVehicle`. The
// `plate` default (empty) selects a fresh per-call license plate;
// pass an explicit value to exercise duplicate-plate paths.
CreateVehicleRequest MakeValidCreateVehicleRequest(std::string plate = "");

// Time range — covers Nov 2023 (the seeded helper range) with margin.
// Helpers in Chunks 3/4 use start.set_seconds(1700000000) = 2023-11-14.
// Default range: 2023-01-01 to 2024-01-01, so any helper-generated row
// is included.
struct TimeRange {
  google::protobuf::Timestamp start;
  google::protobuf::Timestamp end;
};

// 2023-01-01 00:00:00 to 2024-01-01 00:00:00 UTC.
TimeRange DefaultTimeRange();

// Aggregations need data to exist; provide a helper that inserts
// enough charging + consumption rows for one vehicle to make the
// aggregations non-empty.
//
// `n_chargings` and `n_consumptions` default to 3 (enough to exercise
// every aggregation path — charger-type breakdown needs >= 2
// distinct types, source-category breakdown needs >= 1 source row,
// etc.).
void SeedVehicleDataForDisplay(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id,
    int n_chargings = 3,
    int n_consumptions = 3);

}  // namespace evgrpc::test::data
