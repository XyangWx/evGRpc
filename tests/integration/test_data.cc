#include "tests/integration/test_data.h"

#include <stdexcept>

#include "util/uuid.h"

namespace evgrpc::test::data {

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

// NOTE: Chunk 5 forward-deps (DefaultTimeRange, SeedVehicleDataForDisplay)
// were forward-DECLARED in test_data.h but their implementations depend
// on helpers (CreateSourceCategoryId / CreateChargingId / CreateConsumptionId)
// that don't exist yet — they're Chunk 3/4/5 deliverables. The bodies
// live in test_data_display.cc, which is intentionally NOT in the
// CMake source list for evgrpc_integration_tests yet; it'll be added
// when Chunk 5 lands and the underlying helpers exist.

}  // namespace evgrpc::test::data
