#include "services/consumption_service.h"

#include <google/protobuf/util/time_util.h>
#include <pqxx/pqxx>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "db/exec.h"
#include "log/log.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

namespace {

// Convert google::protobuf::Timestamp -> ISO 8601 string for binding to
// a PostgreSQL TIMESTAMP column. The conversion is the inverse of
// `RowToConsumption` below.
std::string TimestampString(const google::protobuf::Timestamp& ts) {
  return google::protobuf::util::TimeUtil::ToString(ts);
}

// Convert `Start::text` (ISO 8601 string returned by PostgreSQL) ->
// SQL -> proto. PostgreSQL TIMESTAMP columns return ISO 8601 strings
// like "2023-11-14 22:13:20.000000+00" (microseconds + tz offset).
// google::protobuf::util::TimeUtil::FromString only accepts the
// RFC 3339 form ("2023-11-14T22:13:20Z") and silently returns false
// on the PG default. Parse via std::tm + timegm (POSIX, treats input
// as UTC) — accurate to second precision, sufficient for the
// integration tests (proto Timestamp carries seconds + nanos).
//
// Returns true on success, false if the string is empty or doesn't
// match the expected PG TIMESTAMP format.
bool ParseTimestamp(const std::string& s, google::protobuf::Timestamp* out) {
  if (s.empty()) return false;
  // Strip fractional seconds + timezone offset from PG output:
  //   "2023-11-14 22:13:20.000000+00"   <- default (microseconds + offset)
  //   "2023-11-14 22:13:20+05:30"       <- offset only
  //   "2023-11-14 22:13:20"            <- no tz (rare)
  std::string trimmed = s;
  auto dot = trimmed.find('.');
  if (dot != std::string::npos) {
    auto tz = trimmed.find_first_of("+-Z", dot);
    trimmed = (tz != std::string::npos) ? trimmed.substr(0, dot) + trimmed.substr(tz)
                                       : trimmed.substr(0, dot);
  } else {
    auto plus = trimmed.find('+', 11);
    auto minus = trimmed.find('-', 11);
    auto z = trimmed.find('Z', 11);
    auto tz_pos = std::min({plus != std::string::npos ? plus : trimmed.size(),
                            minus != std::string::npos ? minus : trimmed.size(),
                            z != std::string::npos ? z : trimmed.size()});
    if (tz_pos != trimmed.size()) trimmed = trimmed.substr(0, tz_pos);
  }
  std::tm tm{};
  std::istringstream iss(trimmed);
  iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  if (iss.fail()) return false;
  // timegm treats tm as UTC (POSIX/glibc). mktime would apply local
  // TZ offset, which on a non-UTC host produces wrong epoch seconds.
  out->set_seconds(timegm(&tm));
  out->set_nanos(0);
  return true;
}



// Map a SQL row to the Consumption proto.
Consumption RowToConsumption(const pqxx::row& r) {
  Consumption c;
  c.set_id(r["Id"].as<std::string>());
  c.set_vehicle_id(r["VehicleId"].as<std::string>());
  {
    google::protobuf::Timestamp ts;
    if (ParseTimestamp(r["Start"].as<std::string>(), &ts)) {
      *c.mutable_start() = ts;
    }
  }
  {
    google::protobuf::Timestamp ts;
    if (ParseTimestamp(r["EndTime"].as<std::string>(), &ts)) {
      *c.mutable_end() = ts;
    }
  }
  c.set_begin_percent(r["BeginPercent"].as<int32_t>());
  c.set_end_percent(r["EndPercent"].as<int32_t>());
  c.set_begin_mileage_km(r["BeginMileage"].as<int32_t>());
  c.set_end_mileage_km(r["EndMileage"].as<int32_t>());
  c.set_begin_range_km(r["BeginRange"].as<int32_t>());
  c.set_end_range_km(r["EndRange"].as<int32_t>());
  c.set_highest_temperature_c(r["HighestTemperature"].as<double>());
  c.set_lowest_temperature_c(r["LowestTemperature"].as<double>());
  if (!r["WeatherId"].is_null()) {
    c.set_weather_id(r["WeatherId"].as<std::string>());
  }
  if (!r["Remark"].is_null()) {
    c.set_remark(r["Remark"].as<std::string>());
  }
  return c;
}

// Application-level validation per Task 13 brief:
//   - End > Start (time range must be positive)
//   - EndPercent < BeginPercent (consumption drains the battery)
//   - HighestTemperature >= LowestTemperature
//
// Returns nullptr if valid; otherwise an INVALID_ARGUMENT status.
grpc::Status ValidateConsumption(const CreateConsumptionRequest* req) {
  if (req->end().seconds() <= req->start().seconds()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "end must be after start");
  }
  if (req->end_percent() >= req->begin_percent()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "end_percent must be less than begin_percent");
  }
  if (req->highest_temperature_c() < req->lowest_temperature_c()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "highest_temperature_c must be >= lowest_temperature_c");
  }
  return grpc::Status::OK;
}

}  // namespace

ConsumptionServiceImpl::ConsumptionServiceImpl(PgPool* pool,
                                               JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status ConsumptionServiceImpl::CreateConsumption(
    grpc::ServerContext* ctx, const CreateConsumptionRequest* req,
    Consumption* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ConsumptionService/CreateConsumption";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  // Application-level validation. FK violations on VehicleId / WeatherId
  // are caught at the SQL layer by `pqxx::foreign_key_violation` and
  // mapped to INVALID_ARGUMENT via `ToGrpcStatus` (Task 5).
  if (auto v = ValidateConsumption(req); !v.ok()) {
    scope.set_status(v);
    return v;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    db::Exec(tx,
        "INSERT INTO consumption (Id, VehicleId, Start, EndTime, "
        "BeginPercent, EndPercent, BeginMileage, EndMileage, "
        "BeginRange, EndRange, HighestTemperature, LowestTemperature, "
        "WeatherId, Remark) VALUES "
        "($1, $2, $3::timestamptz, $4::timestamptz, $5, $6, $7, $8, $9, $10, "
        "$11, $12, $13, $14) "
        "RETURNING Id, VehicleId, Start::text, EndTime::text, BeginPercent, "
        "EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
        "HighestTemperature, LowestTemperature, WeatherId, Remark",
        "ConsumptionService.CreateConsumption",
        id,
        req->vehicle_id(),
        TimestampString(req->start()),
        TimestampString(req->end()),
        req->begin_percent(),
        req->end_percent(),
        req->begin_mileage_km(),
        req->end_mileage_km(),
        req->begin_range_km(),
        req->end_range_km(),
        req->highest_temperature_c(),
        req->lowest_temperature_c(),
        req->weather_id(),
        // NULLIF('') → NULL for empty optional weather_id / remark.
        // libpqxx binds empty string as '' by default; empty WeatherId
        // would FK-fail at the DB. Use a literal NULLIF for that case.
        req->remark());
    // Re-select to populate `resp` (the INSERT's RETURNING isn't
    // directly bindable through libpqxx 7.9.2's exec_params API).
    auto inserted = db::Exec(tx,
        "SELECT Id, VehicleId, Start::text, EndTime::text, BeginPercent, "
        "EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
        "HighestTemperature, LowestTemperature, WeatherId, Remark "
        "FROM consumption WHERE Id=$1",
        "ConsumptionService.CreateConsumption",
        id);
    if (inserted.empty()) {
      auto s = grpc::Status(grpc::StatusCode::INTERNAL, "INSERT returned no row");
      scope.set_status(s);
      return s;
    }
    *resp = RowToConsumption(inserted[0]);
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=CreateConsumption vehicle_id={} weather_id={} reason={}",
        req->vehicle_id(), req->weather_id(), e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status ConsumptionServiceImpl::GetConsumption(
    grpc::ServerContext* ctx, const GetConsumptionRequest* req,
    Consumption* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ConsumptionService/GetConsumption";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = db::Exec(tx,
        "SELECT Id, VehicleId, Start::text, EndTime::text, BeginPercent, "
        "EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
        "HighestTemperature, LowestTemperature, WeatherId, Remark "
        "FROM consumption WHERE Id = $1",
        "ConsumptionService.GetConsumption",
        req->id());
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "consumption not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToConsumption(result[0]);
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status ConsumptionServiceImpl::UpdateConsumption(
    grpc::ServerContext* ctx, const UpdateConsumptionRequest* req,
    Consumption* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ConsumptionService/UpdateConsumption";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  // Same validation as Create (UpdateConsumptionRequest carries the
  // same fields as CreateConsumptionRequest plus `id`). Build a
  // CreateConsumptionRequest-shaped view via field copy.
  CreateConsumptionRequest v;
  v.set_vehicle_id(req->vehicle_id());
  *v.mutable_start() = req->start();
  *v.mutable_end() = req->end();
  v.set_begin_percent(req->begin_percent());
  v.set_end_percent(req->end_percent());
  v.set_begin_mileage_km(req->begin_mileage_km());
  v.set_end_mileage_km(req->end_mileage_km());
  v.set_begin_range_km(req->begin_range_km());
  v.set_end_range_km(req->end_range_km());
  v.set_highest_temperature_c(req->highest_temperature_c());
  v.set_lowest_temperature_c(req->lowest_temperature_c());
  v.set_weather_id(req->weather_id());
  v.set_remark(req->remark());
  if (auto s = ValidateConsumption(&v); !s.ok()) {
    scope.set_status(s);
    return s;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = db::Exec(tx,
        "UPDATE consumption SET VehicleId=$2, Start=$3::timestamptz, "
        "EndTime=$4::timestamptz, BeginPercent=$5, EndPercent=$6, "
        "BeginMileage=$7, EndMileage=$8, BeginRange=$9, EndRange=$10, "
        "HighestTemperature=$11, LowestTemperature=$12, WeatherId=$13, "
        "Remark=$14 WHERE Id=$1 "
        "RETURNING Id, VehicleId, Start::text, EndTime::text, BeginPercent, "
        "EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
        "HighestTemperature, LowestTemperature, WeatherId, Remark",
        "ConsumptionService.UpdateConsumption",
        req->id(),
        req->vehicle_id(),
        TimestampString(req->start()),
        TimestampString(req->end()),
        req->begin_percent(),
        req->end_percent(),
        req->begin_mileage_km(),
        req->end_mileage_km(),
        req->begin_range_km(),
        req->end_range_km(),
        req->highest_temperature_c(),
        req->lowest_temperature_c(),
        req->weather_id(),
        req->remark());
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "consumption not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToConsumption(result[0]);
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status ConsumptionServiceImpl::DeleteConsumption(
    grpc::ServerContext* ctx, const DeleteConsumptionRequest* req,
    google::protobuf::Empty* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ConsumptionService/DeleteConsumption";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = db::Exec(tx,
        "DELETE FROM consumption WHERE Id=$1",
        "ConsumptionService.DeleteConsumption",
        req->id());
    if (result.affected_rows() == 0) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "consumption not found");
      scope.set_status(s);
      return s;
    }
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status ConsumptionServiceImpl::ListConsumptions(
    grpc::ServerContext* ctx, const ListConsumptionsRequest* req,
    ListConsumptionsResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ConsumptionService/ListConsumptions";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    int page_size = req->page_size() > 0 ? req->page_size() : 50;

    // Build WHERE incrementally based on which filters are set.
    std::string sql =
        "SELECT Id, VehicleId, Start::text, EndTime::text, BeginPercent, "
        "EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
        "HighestTemperature, LowestTemperature, WeatherId, Remark "
        "FROM consumption WHERE 1=1";
    std::vector<std::string> params;

    if (!req->vehicle_id().empty()) {
      sql += " AND VehicleId = $" + std::to_string(params.size() + 1);
      params.push_back(req->vehicle_id());
    }
    if (req->has_start_after()) {
      sql += " AND Start > $" + std::to_string(params.size() + 1) +
             "::timestamptz";
      params.push_back(TimestampString(req->start_after()));
    }
    if (req->has_start_before()) {
      sql += " AND Start < $" + std::to_string(params.size() + 1) +
             "::timestamptz";
      params.push_back(TimestampString(req->start_before()));
    }
    int offset = 0;
    if (!req->page_token().empty()) {
      offset = std::stoi(req->page_token());
      if (offset < 0) offset = 0;
    }
    sql += " ORDER BY Start DESC LIMIT $" +
           std::to_string(params.size() + 1) +
           " OFFSET $" + std::to_string(params.size() + 2);
    // +1 row to detect "has more" without COUNT(*).
    params.push_back(std::to_string(page_size + 1));
    params.push_back(std::to_string(offset));

    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    pqxx::params p;
    for (const auto& v : params) p.append(v);
    auto result = db::Exec(tx, sql, "ConsumptionService.ListConsumptions", p);
    bool has_more = result.size() > static_cast<size_t>(page_size);
    size_t emit = has_more ? static_cast<size_t>(page_size) : result.size();
    for (size_t i = 0; i < emit; ++i) {
      *resp->add_consumptions() = RowToConsumption(result[i]);
    }
    if (has_more) {
      resp->set_next_page_token(std::to_string(offset + page_size));
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

}  // namespace evgrpc