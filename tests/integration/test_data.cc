#include "tests/integration/test_data.h"

#include <stdexcept>

#include "evgrpc/vehicle.grpc.pb.h"
#include "evgrpc/source_category.grpc.pb.h"
#include "evgrpc/charging.grpc.pb.h"
#include "evgrpc/consumption.grpc.pb.h"
#include "util/uuid.h"

namespace evgrpc::test::data {

// File-scope (translation-unit-private) field-copy helper for
// ToUpdateChargingRequest. Kept in an anonymous namespace because
// nothing outside this TU should know about the 14-field mapping —
// if a proto field is added, we'd update CopyToUpdateRequest and
// nothing else.
namespace {

void CopyToUpdateRequest(const CreateChargingRequest& src,
                         UpdateChargingRequest* dst) {
  dst->set_vehicle_id(src.vehicle_id());
  // proto3 singular message fields don't get `set_X(Message)` — only
  // `mutable_X()` returning a pointer. Assign through the pointer to
  // preserve the has-bit semantics on the wrapper field.
  *dst->mutable_start_time() = src.start_time();
  *dst->mutable_end_time() = src.end_time();
  dst->set_start_percent(src.start_percent());
  dst->set_end_percent(src.end_percent());
  dst->set_start_mileage_km(src.start_mileage_km());
  dst->set_end_mileage_km(src.end_mileage_km());
  dst->set_kwh_charged(src.kwh_charged());
  dst->set_cost(src.cost());
  dst->set_electricity_unit_price(src.electricity_unit_price());
  // ServiceFee is a DoubleValue wrapper (nullable). has-bit must be
  // propagated explicitly so callers that set it round-trip back to
  // NULL when src is unset.
  if (src.has_service_fee()) {
    *dst->mutable_service_fee() = src.service_fee();
  }
  dst->set_charger_type(src.charger_type());
  dst->set_source_category_id(src.source_category_id());
  dst->set_location(src.location());
  dst->set_remark(src.remark());
}

}  // namespace

std::string FreshUuid() { return evgrpc::NewUuid(); }

std::string FreshLicensePlate() {
  return "T-" + FreshUuid().substr(0, 8);
}

Vehicle MakeValidVehicle(std::string plate) {
  Vehicle v;
  v.set_id(FreshUuid());
  v.set_brand("Tesla");
  v.set_calibrated_range_km(500);
  v.set_battery_capacity_kwh(75.0);
  google::protobuf::Timestamp ts;
  ts.set_seconds(1700000000);   // 2023-11-14 22:13:20 UTC
  *v.mutable_purchase_date() = ts;
  v.set_license_plate(plate.empty() ? FreshLicensePlate() : plate);
  return v;
}

CreateVehicleRequest MakeValidCreateVehicleRequest(std::string plate) {
  CreateVehicleRequest req;
  req.set_brand("Tesla");
  req.set_calibrated_range_km(500);
  req.set_battery_capacity_kwh(75.0);
  google::protobuf::Timestamp ts;
  ts.set_seconds(1700000000);   // 2023-11-14 22:13:20 UTC
  *req.mutable_purchase_date() = ts;
  req.set_license_plate(plate.empty() ? FreshLicensePlate() : plate);
  return req;
}

// ---- ChargingService setup helpers (Chunk 3, Task 15) ----
//
// Round-trip real gRPC requests through the TestServer so tests get
// server-set ids. Any non-OK status throws std::runtime_error —
// never use abseil/glog CHECK here (commit `8aaf260` removed it).

std::string CreateVehicleId(std::shared_ptr<grpc::Channel> channel) {
  auto stub = VehicleService::NewStub(channel);
  const auto req = MakeValidCreateVehicleRequest();
  Vehicle resp;
  grpc::ClientContext ctx;
  const auto status = stub->CreateVehicle(&ctx, req, &resp);
  if (!status.ok()) {
    throw std::runtime_error(
        "CreateVehicleId: gRPC failed: " + status.error_message());
  }
  return resp.id();
}

std::string CreateSourceCategoryId(std::shared_ptr<grpc::Channel> channel) {
  auto stub = SourceCategoryService::NewStub(channel);
  CreateSourceCategoryRequest req;
  // Fresh per-call name so the table's UNIQUE constraint can never
  // collide across tests / runs (UNIQUE on `name` in the
  // source_category table).
  req.set_name("SC-" + FreshUuid().substr(0, 8));
  SourceCategory resp;
  grpc::ClientContext ctx;
  const auto status = stub->CreateSourceCategory(&ctx, req, &resp);
  if (!status.ok()) {
    throw std::runtime_error(
        "CreateSourceCategoryId: gRPC failed: " + status.error_message());
  }
  return resp.id();
}

std::string CreateChargingId(
    std::shared_ptr<grpc::Channel> channel,
    const std::string& vehicle_id,
    const std::string& source_category_id) {
  auto stub = ChargingService::NewStub(channel);
  const auto req =
      MakeValidCreateChargingRequest(vehicle_id, source_category_id);
  Charging resp;
  grpc::ClientContext ctx;
  const auto status = stub->CreateCharging(&ctx, req, &resp);
  if (!status.ok()) {
    throw std::runtime_error(
        "CreateChargingId: gRPC failed: " + status.error_message());
  }
  return resp.id();
}

CreateChargingRequest MakeValidCreateChargingRequest(
    const std::string& vehicle_id,
    const std::string& source_category_id) {
  CreateChargingRequest req;
  req.set_vehicle_id(vehicle_id);
  req.set_source_category_id(source_category_id);
  // 2023-11-14 22:13:20 UTC (= 1700000000). The plus-3600 (=1h) end
  // time satisfies ValidateCharging(end_time > start_time).
  google::protobuf::Timestamp start;
  start.set_seconds(1700000000);
  google::protobuf::Timestamp end;
  end.set_seconds(1700003600);
  *req.mutable_start_time() = start;
  *req.mutable_end_time() = end;
  // Charging ADDS charge, so end_percent > start_percent (opposite
  // of consumption). 20 -> 80 satisfies ValidateCharging.
  req.set_start_percent(20);
  req.set_end_percent(80);
  req.set_start_mileage_km(10000);
  req.set_end_mileage_km(10100);
  req.set_kwh_charged(50.0);
  req.set_cost(75.0);
  req.set_electricity_unit_price(1.5);
  // service_fee intentionally left unset (DoubleValue wrapper has-bit
  // false -> column is NULL).
  req.set_charger_type(CHARGER_TYPE_FAST);
  req.set_location("Home");
  // remark intentionally left empty (-> column NULL).
  return req;
}

UpdateChargingRequest ToUpdateChargingRequest(
    const CreateChargingRequest& src) {
  UpdateChargingRequest dst;
  // id is NOT copied: CreateChargingRequest has no id field (the
  // server sets it on Create). The caller must `dst.set_id(...)`
  // before sending through ChargingService.UpdateCharging.
  CopyToUpdateRequest(src, &dst);
  return dst;
}

// ---- ConsumptionService setup helpers (Chunk 4, Task 23) ----
//
// Round-trip real gRPC requests through the TestServer so tests get
// server-set ids. Any non-OK status throws std::runtime_error —
// never use abseil/glog CHECK here (commit `8aaf260` removed it).
//
// CreateWeatherId uses DIRECT SQL (not the gRPC surface) because
// WeatherService isn't implemented until Chunk 6 — Chunk 4 has no
// cross-chunk dependency this way.

std::string CreateWeatherId(std::shared_ptr<PgContainer> pg) {
  // Match `sql/001_initial.sql:18-21` weather table schema:
  //   Id    UUID PRIMARY KEY,
  //   Name  VARCHAR(36) NOT NULL UNIQUE
  auto conn = std::make_shared<pqxx::connection>(pg->Conninfo());
  pqxx::work tx(*conn);
  const std::string id = FreshUuid();
  tx.exec_params(
      "INSERT INTO weather (Id, Name) VALUES ($1, $2)",
      id, "W-" + id.substr(0, 8));
  tx.commit();
  return id;
}

std::string CreateConsumptionId(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id) {
  const auto wid = CreateWeatherId(pg);  // auto-create FK prerequisite
  auto stub = ConsumptionService::NewStub(channel);
  const auto req = MakeValidCreateConsumptionRequest(vehicle_id, wid);
  Consumption resp;
  grpc::ClientContext ctx;
  const auto status = stub->CreateConsumption(&ctx, req, &resp);
  if (!status.ok()) {
    throw std::runtime_error(
        "CreateConsumptionId: gRPC failed: " + status.error_message());
  }
  return resp.id();
}

CreateConsumptionRequest MakeValidCreateConsumptionRequest(
    const std::string& vehicle_id,
    const std::string& weather_id) {
  CreateConsumptionRequest req;
  req.set_vehicle_id(vehicle_id);
  req.set_weather_id(weather_id);  // REQUIRED — no default
  // 2023-11-14 22:13:20 UTC (= 1700000000). The plus-3600 (=1h) end
  // time satisfies ValidateConsumption(end > start).
  google::protobuf::Timestamp start;
  start.set_seconds(1700000000);
  google::protobuf::Timestamp end;
  end.set_seconds(1700003600);
  // proto3 singular message fields don't get `set_X(Message)` — only
  // `mutable_X()` returning a pointer. Assign through the pointer.
  *req.mutable_start() = start;
  *req.mutable_end() = end;
  // Consumption DRAINS charge, so end_percent < begin_percent
  // (inverse of charging). 80 -> 20 satisfies ValidateConsumption.
  req.set_begin_percent(80);
  req.set_end_percent(20);
  req.set_begin_mileage_km(10000);
  req.set_end_mileage_km(10100);
  req.set_begin_range_km(400);
  req.set_end_range_km(350);
  // highest >= lowest satisfies ValidateConsumption.
  req.set_highest_temperature_c(25.0);
  req.set_lowest_temperature_c(10.0);
  // remark intentionally left empty (-> column NULL).
  return req;
}

void CopyToUpdateRequest(const CreateConsumptionRequest& src,
                         UpdateConsumptionRequest* dst) {
  dst->set_vehicle_id(src.vehicle_id());
  *dst->mutable_start() = src.start();
  *dst->mutable_end() = src.end();
  dst->set_begin_percent(src.begin_percent());
  dst->set_end_percent(src.end_percent());
  dst->set_begin_mileage_km(src.begin_mileage_km());
  dst->set_end_mileage_km(src.end_mileage_km());
  dst->set_begin_range_km(src.begin_range_km());
  dst->set_end_range_km(src.end_range_km());
  dst->set_highest_temperature_c(src.highest_temperature_c());
  dst->set_lowest_temperature_c(src.lowest_temperature_c());
  dst->set_weather_id(src.weather_id());
  dst->set_remark(src.remark());
}

UpdateConsumptionRequest ToUpdateConsumptionRequest(
    const CreateConsumptionRequest& src) {
  UpdateConsumptionRequest dst;
  // id is NOT copied: CreateConsumptionRequest has no id field (the
  // server sets it on Create). The caller must `dst.set_id(...)`
  // before sending through ConsumptionService.UpdateConsumption.
  CopyToUpdateRequest(src, &dst);
  return dst;
}

// NOTE: Chunk 5 forward-deps (DefaultTimeRange, SeedVehicleDataForDisplay)
// were forward-DECLARED in test_data.h but their bodies depend on
// helpers that don't all exist yet:
//   * CreateSourceCategoryId  — implemented in this TU (Chunk 3 Task 15)
//   * CreateChargingId        — implemented in this TU (Chunk 3 Task 15)
//   * CreateConsumptionId     — not yet implemented (Chunk 4)
// The DefaultTimeRange + SeedVehicleDataForDisplay bodies therefore
// still live in test_data_display.cc, which remains intentionally NOT
// in the CMake source list for evgrpc_integration_tests until Chunk 5
// lands and CreateConsumptionId is added.

}  // namespace evgrpc::test::data
