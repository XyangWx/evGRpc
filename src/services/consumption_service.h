#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/consumption.pb.h"
#include "evgrpc/consumption.grpc.pb.h"

namespace evgrpc {

// Concrete implementation of the generated `ConsumptionService::Service`
// from `proto/evgrpc/consumption.proto`. 5 RPCs (CRUD + List) backed by
// the `consumption` table (Task 3's DDL).
//
// Same prologue as Task 10/11/12: `AuthenticateRpc` + `RpcScope` with
// shared req_id (Task 10.5). Application-level checks live in
// `ValidateConsumption` below: end > start, end_percent < begin_percent
// (consumption drains the battery), highest_temp >= lowest_temp.
class ConsumptionServiceImpl final : public ConsumptionService::Service {
 public:
  ConsumptionServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status CreateConsumption(grpc::ServerContext*,
                                  const CreateConsumptionRequest*,
                                  Consumption*) override;
  grpc::Status GetConsumption(grpc::ServerContext*,
                               const GetConsumptionRequest*,
                               Consumption*) override;
  grpc::Status UpdateConsumption(grpc::ServerContext*,
                                  const UpdateConsumptionRequest*,
                                  Consumption*) override;
  grpc::Status DeleteConsumption(grpc::ServerContext*,
                                  const DeleteConsumptionRequest*,
                                  google::protobuf::Empty*) override;
  grpc::Status ListConsumptions(grpc::ServerContext*,
                                 const ListConsumptionsRequest*,
                                 ListConsumptionsResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc