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
#include <pqxx/pqxx>

#include "evgrpc/vehicle.pb.h"
#include "evgrpc/charging.pb.h"             // CreateChargingRequest, UpdateChargingRequest
#include "evgrpc/consumption.pb.h"           // CreateConsumptionRequest, UpdateConsumptionRequest
#include "evgrpc/source_category.pb.h"       // CreateSourceCategoryRequest
#include "evgrpc/display.pb.h"               // PeriodReport, VehicleCostSummary, list responses
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

// Build a fully-valid Vehicle proto for expected-shape fixtures (e.g.,
// comparing server `GetVehicle` response field-by-field). For
// `CreateVehicle` RPC input, use `MakeValidCreateVehicleRequest()`
// instead — Vehicle and CreateVehicleRequest are distinct proto types
// with no implicit conversion.
Vehicle MakeValidVehicle(std::string plate = "");

// Build a CreateVehicleRequest — Vehicle fields minus the server-set
// `id`. Used as RPC input to `VehicleService.CreateVehicle`. The
// `plate` default (empty) selects a fresh per-call license plate;
// pass an explicit value to exercise duplicate-plate paths.
CreateVehicleRequest MakeValidCreateVehicleRequest(std::string plate = "");

// ---- ChargingService setup helpers (Chunk 3, Task 15) ----
//
// These wrap the per-test gRPC boilerplate so Chunks 3/4/5 tests
// don't repeat the stub/ClientContext/CHECK dance on every fixture
// setup. All three round-trip a request through the running
// TestServer and return the server-set id, so the caller never has
// to inspect gRPC statuses for the happy path. Any non-OK status
// throws std::runtime_error (per Chunk 1 Task 5 fix at commit
// `8aaf260` — abseil/glog CHECK macro is not defined here).

// Create a vehicle row via VehicleService.CreateVehicle gRPC and
// return its server-set id. Used as the FK prerequisite for
// CreateCharging below (vehicle.charging.vehicle_id REFERENCES
// vehicle.id).
std::string CreateVehicleId(std::shared_ptr<grpc::Channel> channel);

// Create a source_category row via SourceCategoryService.CreateSourceCategory
// gRPC and return its server-set id. Used as the FK prerequisite for
// CreateCharging below.
std::string CreateSourceCategoryId(std::shared_ptr<grpc::Channel> channel);

// Create a charging row via ChargingService.CreateCharging gRPC and
// return its server-set id. `vehicle_id` and `source_category_id`
// must already exist (CreateVehicle + CreateSourceCategoryId).
std::string CreateChargingId(
    std::shared_ptr<grpc::Channel> channel,
    const std::string& vehicle_id,
    const std::string& source_category_id);

// Build a fully-valid CreateChargingRequest that passes the
// production `ValidateCharging` (end_time > start_time,
// end_percent > start_percent, kwh_charged > 0, cost > 0):
//   * vehicle_id / source_category_id — caller-supplied FK refs
//   * start_time = 2023-11-14 22:13:20 UTC (1700000000)
//   * end_time   = +1h (1700003600)
//   * start_percent=20, end_percent=80 (charging ADDS charge)
//   * start_mileage_km=10000, end_mileage_km=10100 (charging grows
//     mileage)
//   * kwh_charged=50.0, cost=75.0, electricity_unit_price=1.5
//   * service_fee unset (DoubleValue wrapper has-bit false)
//   * charger_type=CHARGER_TYPE_FAST
//   * location="Home", remark="" (test can override as needed)
CreateChargingRequest MakeValidCreateChargingRequest(
    const std::string& vehicle_id,
    const std::string& source_category_id);

// Convert a CreateChargingRequest into an UpdateChargingRequest by
// copying every non-id field (vehicle_id / start_time / end_time /
// start_percent / end_percent / start_mileage_km / end_mileage_km /
// kwh_charged / cost / electricity_unit_price / service_fee /
// charger_type / source_category_id / location / remark).
//
// `id` is NOT copied — CreateChargingRequest has no id field (it's
// server-set), so the caller must `set_id` on the result before
// calling ChargingService.UpdateCharging. Used by Update tests
// (Tasks 18/19) to avoid 14-field copy-paste.
UpdateChargingRequest ToUpdateChargingRequest(const CreateChargingRequest& src);

// ---- ConsumptionService setup helpers (Chunk 4, Task 23) ----
//
// `weather_id` is REQUIRED by the DB schema (`consumption.WeatherId`
// is NOT NULL with FK REFERENCES weather(Id) per sql/001_initial.sql:26).
// There is NO default — callers must supply a real weather id. The
// auto-creating `CreateConsumptionId` helper below creates one via
// direct SQL so we don't pull in WeatherService (Chunk 6).

// Direct-SQL weather row creator. Avoids cross-chunk dependency on
// WeatherService (Chunk 6). Returns the new weather Id.
std::string CreateWeatherId(std::shared_ptr<PgContainer> pg);

// Convenience: auto-creates a weather row, then a consumption row.
// Returns the consumption id. Use this in tests that don't need to
// assert on a specific weather_id.
std::string CreateConsumptionId(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id);

// Build a fully-valid CreateConsumptionRequest that passes the
// production `ValidateConsumption` (end > start,
// end_percent < begin_percent, highest_temperature_c >=
// lowest_temperature_c):
//   * vehicle_id / weather_id — caller-supplied FK refs
//   * start = 2023-11-14 22:13:20 UTC (1700000000)
//   * end   = +1h (1700003600)
//   * begin_percent=80, end_percent=20 (consumption DRAINS charge,
//     inverse of charging)
//   * begin_mileage_km=10000, end_mileage_km=10100
//   * begin_range_km=400, end_range_km=350 (range shrinks)
//   * highest_temperature_c=25.0, lowest_temperature_c=10.0
//   * remark empty (-> column NULL)
CreateConsumptionRequest MakeValidCreateConsumptionRequest(
    const std::string& vehicle_id,
    const std::string& weather_id);

// Convert a CreateConsumptionRequest into an UpdateConsumptionRequest by
// copying every non-id field (vehicle_id / start / end / begin_percent /
// end_percent / begin_mileage_km / end_mileage_km / begin_range_km /
// end_range_km / highest_temperature_c / lowest_temperature_c /
// weather_id / remark).
//
// `id` is NOT copied — CreateConsumptionRequest has no id field (it's
// server-set), so the caller must `set_id` on the result before
// calling ConsumptionService.UpdateConsumption. Used by Update tests
// (Task 26) to avoid 13-field copy-paste.
UpdateConsumptionRequest ToUpdateConsumptionRequest(
    const CreateConsumptionRequest& src);

// ---- DisplayService helpers (Chunk 5, Task 31) ----
//
// `DefaultTimeRange` covers 2023-01-01 to 2024-01-01 so any helper-
// generated charging/consumption row (which use 2023-11-14 as their
// timestamp) is inside the window. `SeedVehicleDataForDisplay`
// populates enough charging + consumption rows to make every
// aggregation RPC non-empty (3 of each is enough to exercise both
// charger-type breakdowns and monthly aggregations).

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
