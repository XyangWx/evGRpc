#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"

namespace evgrpc {

// Concrete implementation of the generated `VehicleService::Service`
// from `proto/evgrpc/vehicle.proto`. Backed by `PgPool` (Task 4) for
// storage and `JwtValidator` (Task 7) for auth.
//
// Task 9 prologue: every RPC body calls `evgrpc::Authenticate(ctx->
// client_metadata(), *validator_)` as its first DB-touching action.
// Task 9.5 logging: every RPC body holds an `RpcScope` on the stack
// for structured entry/exit logging (spec §5.6).
class VehicleServiceImpl final : public VehicleService::Service {
 public:
  VehicleServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status CreateVehicle(grpc::ServerContext*, const CreateVehicleRequest*,
                              Vehicle*) override;
  grpc::Status GetVehicle(grpc::ServerContext*, const GetVehicleRequest*,
                           Vehicle*) override;
  grpc::Status UpdateVehicle(grpc::ServerContext*, const UpdateVehicleRequest*,
                              Vehicle*) override;
  grpc::Status DeleteVehicle(grpc::ServerContext*, const DeleteVehicleRequest*,
                              google::protobuf::Empty*) override;
  grpc::Status ListVehicles(grpc::ServerContext*, const ListVehiclesRequest*,
                             ListVehiclesResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc