#include "services/weather_service.h"

#include <pqxx/pqxx>
#include "auth/authenticate.h"
#include "db/error.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

WeatherServiceImpl::WeatherServiceImpl(PgPool* pool, JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status WeatherServiceImpl::CreateWeather(
    grpc::ServerContext* ctx, const CreateWeatherRequest* req, Weather* resp) {
  RpcScope scope("/evgrpc.WeatherService/CreateWeather",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    tx.exec_params(
        "INSERT INTO weather (Id, Name) VALUES ($1, $2)",
        id, req->name());
    tx.commit();

    resp->set_id(id);
    resp->set_name(req->name());
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status WeatherServiceImpl::SearchWeather(
    grpc::ServerContext* ctx, const SearchWeatherRequest* req,
    SearchWeatherResponse* resp) {
  RpcScope scope("/evgrpc.WeatherService/SearchWeather",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    // Default limit 50 — matches Task 10's ListVehicles default and
    // keeps autocomplete responses snappy.
    int limit = req->limit() > 0 ? req->limit() : 50;

    // PostgreSQL's `^@` operator: case-sensitive "starts with". Unlike
    // LIKE, `%` and `_` are not wildcards here — user input is safe to
    // bind directly without escaping. Empty prefix matches everything
    // (bounded by limit), which is the desired autocomplete behavior
    // when the user has typed nothing.
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = tx.exec_params(
        "SELECT Id, Name FROM weather WHERE Name ^@ $1 "
        "ORDER BY Name LIMIT $2",
        req->prefix(), limit);

    for (const auto& row : result) {
      auto* w = resp->add_matches();
      w->set_id(row["Id"].as<std::string>());
      w->set_name(row["Name"].as<std::string>());
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

}  // namespace evgrpc