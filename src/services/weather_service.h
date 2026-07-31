#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/weather.pb.h"
#include "evgrpc/weather.grpc.pb.h"

namespace evgrpc {

// Concrete implementation of the generated `WeatherService::Service`
// from `proto/evgrpc/weather.proto`. Backed by `PgPool` (Task 4) for
// storage and `JwtValidator` (Task 7) for auth.
//
// 2 RPCs:
//   CreateWeather — server-mint UUID, INSERT into `weather`,
//                    return the new row.
//   SearchWeather — prefix-match on `Name` using PostgreSQL's `^@`
//                    operator (case-sensitive starts-with; no LIKE
//                    escape needed because `%` and `_` aren't wildcards).
//
// Same prologue as Task 10's VehicleService (Authenticate + RpcScope).
class WeatherServiceImpl final : public WeatherService::Service {
 public:
  WeatherServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status CreateWeather(grpc::ServerContext*, const CreateWeatherRequest*,
                              Weather*) override;
  grpc::Status SearchWeather(grpc::ServerContext*, const SearchWeatherRequest*,
                              SearchWeatherResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc