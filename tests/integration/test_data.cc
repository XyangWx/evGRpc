#include "tests/integration/test_data.h"

#include <stdexcept>

namespace evgrpc::test::data {

TimeRange DefaultTimeRange() {
  // Wide range covering any data the helpers (Chunks 3/4) seed.
  // Chunk 3/4 helpers use start.set_seconds(1700000000) = 2023-11-14.
  // Use 2023-01-01 to 2024-01-01 so any helper-generated row is included.
  TimeRange r;
  r.start.set_seconds(1672531200);   // 2023-01-01 00:00:00 UTC
  r.end.set_seconds(1704067200);     // 2024-01-01 00:00:00 UTC
  return r;
}

void SeedVehicleDataForDisplay(
    std::shared_ptr<grpc::Channel> channel,
    std::shared_ptr<PgContainer> pg,
    const std::string& vehicle_id,
    int n_chargings,
    int n_consumptions) {
  const auto sid = CreateSourceCategoryId(channel);
  for (int i = 0; i < n_chargings; ++i) {
    const auto cid = CreateChargingId(channel, vehicle_id, sid);
    if (cid.empty()) {
      throw std::runtime_error("SeedVehicleDataForDisplay: CreateChargingId returned empty");
    }
  }
  for (int i = 0; i < n_consumptions; ++i) {
    const auto cid = CreateConsumptionId(channel, pg, vehicle_id);
    if (cid.empty()) {
      throw std::runtime_error("SeedVehicleDataForDisplay: CreateConsumptionId returned empty");
    }
  }
}

}  // namespace evgrpc::test::data
