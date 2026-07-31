#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/display.pb.h"
#include "evgrpc/display.grpc.pb.h"

namespace evgrpc {

// DisplayService — read-only aggregation queries over the charging +
// consumption tables. No state; pure SQL SUM/AVG/CTE.
//
// Tasks 16–19 split into 4 commits (3+2+2+1 RPCs); this file covers
// Task 16's 3 RPCs:
//   - GetVehicleCostSummary  (total cost / kWh / avg 元 per kWh + 元 per km)
//   - GetMonthlyReport       (one month, one vehicle or all)
//   - GetAnnualReport        (one year,  one vehicle or all)
//
// Same prologue as Task 10/11/12/13/14: `AuthenticateRpc` + `RpcScope`
// with shared req_id (Task 10.5).
class DisplayServiceImpl final : public DisplayService::Service {
 public:
  DisplayServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status GetVehicleCostSummary(
      grpc::ServerContext*, const GetVehicleCostSummaryRequest*,
      VehicleCostSummary*) override;
  grpc::Status GetMonthlyReport(grpc::ServerContext*,
                                 const GetMonthlyReportRequest*,
                                 PeriodReport*) override;
  grpc::Status GetAnnualReport(grpc::ServerContext*,
                                const GetAnnualReportRequest*,
                                PeriodReport*) override;
  grpc::Status GetCostByChargerType(
      grpc::ServerContext*, const GetCostByChargerTypeRequest*,
      GetCostByChargerTypeResponse*) override;
  grpc::Status GetCostBySourceCategory(
      grpc::ServerContext*, const GetCostBySourceCategoryRequest*,
      GetCostBySourceCategoryResponse*) override;
  grpc::Status GetConsumptionEfficiency(
      grpc::ServerContext*, const GetConsumptionEfficiencyRequest*,
      GetConsumptionEfficiencyResponse*) override;
  grpc::Status GetRangeAccuracy(grpc::ServerContext*,
                                const GetRangeAccuracyRequest*,
                                GetRangeAccuracyResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc