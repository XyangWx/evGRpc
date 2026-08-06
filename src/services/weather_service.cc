#include "services/weather_service.h"

#include <pqxx/pqxx>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "db/exec.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

WeatherServiceImpl::WeatherServiceImpl(PgPool* pool, JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status WeatherServiceImpl::CreateWeather(
    grpc::ServerContext* ctx, const CreateWeatherRequest* req, Weather* resp) {
  static constexpr const char* kMethod = "/evgrpc.WeatherService/CreateWeather";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    db::Exec(tx,
        "INSERT INTO weather (Id, Name) VALUES ($1, $2)",
        "WeatherService.CreateWeather",
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
  static constexpr const char* kMethod = "/evgrpc.WeatherService/SearchWeather";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

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
    auto result = db::Exec(tx,
        "SELECT Id, Name FROM weather WHERE Name ^@ $1 "
        "ORDER BY Name LIMIT $2",
        "WeatherService.SearchWeather",
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