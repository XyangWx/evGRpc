#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/charging.pb.h"
#include "evgrpc/charging.grpc.pb.h"

namespace evgrpc {

// Concrete implementation of the generated `ChargingService::Service`
// from `proto/evgrpc/charging.proto`. 5 RPCs (CRUD + List) backed by
// the `charging` table (FK to vehicle + source_category, TIMESTAMP
// columns, DECIMAL amounts, optional ServiceFee, ChargerType enum).
//
// Same prologue as Task 10/11/12/13: `AuthenticateRpc` + `RpcScope`
// with shared req_id (Task 10.5). Application-level checks live in
// `ValidateCharging` below: end > start, end_percent > start_percent
// (charging is a gain, not a drain), kwh_charged > 0, cost > 0.
class ChargingServiceImpl final : public ChargingService::Service {
 public:
  ChargingServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status CreateCharging(grpc::ServerContext*,
                              const CreateChargingRequest*,
                              Charging*) override;
  grpc::Status GetCharging(grpc::ServerContext*,
                           const GetChargingRequest*,
                           Charging*) override;
  grpc::Status UpdateCharging(grpc::ServerContext*,
                              const UpdateChargingRequest*,
                              Charging*) override;
  grpc::Status DeleteCharging(grpc::ServerContext*,
                              const DeleteChargingRequest*,
                              google::protobuf::Empty*) override;
  grpc::Status ListChargings(grpc::ServerContext*,
                             const ListChargingsRequest*,
                             ListChargingsResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc