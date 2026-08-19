#include "services/charging_service.h"

#include <google/protobuf/util/time_util.h>
#include <pqxx/pqxx>
#include <string>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "db/exec.h"
#include "log/log.h"
#include "services/charger/charger_type.h"
#include "util/page_token_parse.h"
#include "util/rpc_scope.h"
#include "util/timestamp_parse.h"
#include "util/uuid.h"

namespace evgrpc {

namespace {

// proto -> SQL
std::string TimestampString(const google::protobuf::Timestamp& ts) {
  return google::protobuf::util::TimeUtil::ToString(ts);
}

// Map a SQL row to the Charging proto.
Charging RowToCharging(const pqxx::row& r) {
  Charging c;
  c.set_id(r["Id"].as<std::string>());
  c.set_vehicle_id(r["VehicleId"].as<std::string>());
  {
    google::protobuf::Timestamp ts;
    if (ParseTimestamp(r["StartTime"].as<std::string>(), &ts)) {
      *c.mutable_start_time() = ts;
    }
  }
  {
    google::protobuf::Timestamp ts;
    if (ParseTimestamp(r["EndTime"].as<std::string>(), &ts)) {
      *c.mutable_end_time() = ts;
    }
  }
  c.set_start_percent(r["StartPercent"].as<int32_t>());
  c.set_end_percent(r["EndPercent"].as<int32_t>());
  c.set_start_mileage_km(r["StartMileage"].as<int32_t>());
  c.set_end_mileage_km(r["EndMileage"].as<int32_t>());
  c.set_kwh_charged(r["KwhCharged"].as<double>());
  c.set_cost(r["Cost"].as<double>());
  c.set_electricity_unit_price(r["ElectricityUnitPrice"].as<double>());
  if (!r["ServiceFee"].is_null()) {
    c.mutable_service_fee()->set_value(r["ServiceFee"].as<double>());
  }
  c.set_charger_type(ChargerTypeFromLabel(r["ChargerType"].as<std::string>()));
  c.set_source_category_id(r["SourceCategoryId"].as<std::string>());
  if (!r["Location"].is_null()) {
    c.set_location(r["Location"].as<std::string>());
  }
  if (!r["Remark"].is_null()) {
    c.set_remark(r["Remark"].as<std::string>());
  }
  return c;
}

// Application-level validation per Task 14 brief:
//   - end > start (time range must be positive)
//   - end_percent > start_percent (charging ADDS charge; opposite of
//     consumption)
//   - kwh_charged > 0
//   - cost > 0
grpc::Status ValidateCharging(const CreateChargingRequest* req) {
  if (req->end_time().seconds() <= req->start_time().seconds()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "end_time must be after start_time");
  }
  if (req->end_percent() <= req->start_percent()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "end_percent must be greater than start_percent");
  }
  if (req->kwh_charged() <= 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "kwh_charged must be > 0");
  }
  if (req->cost() <= 0) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "cost must be > 0");
  }
  // ChargerType must be a valid enum value (FAST or SLOW). UNSPECIFIED
  // (= 0) is the proto3 default and not a real value; binding it to the
  // DB enum column would fail with NOT NULL (because ChargerTypeLabel
  // returns '' for UNSPECIFIED).
  if (req->charger_type() == ChargerType::CHARGER_TYPE_UNSPECIFIED) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "charger_type must be FAST or SLOW (not UNSPECIFIED)");
  }
  return grpc::Status::OK;
}

}  // namespace

ChargingServiceImpl::ChargingServiceImpl(PgPool* pool, JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

// Helper: bind NULL for proto wrapper-types that aren't set, empty
// string for proto strings that are empty, etc. Returns a tuple-shaped
// pair (or just inline at the call site). For now we just pass empty
// strings/0.0/nullopt into the SQL and let PostgreSQL column defaults
// + the NOT NULL constraints on VehicleId/SourceCategoryId do the rest.
namespace {
// Empty string as bound for ServiceFee (NULL) and Location / Remark (NULL).
// pqxx binds nullptr_t as SQL NULL. We use the helper for the explicit
// three nullable string fields and the optional double.
template <typename T>
pqxx::params ChargeParams(T req_id, const CreateChargingRequest* req) {
  pqxx::params p;
  p.append(req_id);
  p.append(req->vehicle_id());
  p.append(TimestampString(req->start_time()));
  p.append(TimestampString(req->end_time()));
  p.append(req->start_percent());
  p.append(req->end_percent());
  p.append(req->start_mileage_km());
  p.append(req->end_mileage_km());
  p.append(req->kwh_charged());
  p.append(req->cost());
  p.append(req->electricity_unit_price());
  // ServiceFee is DoubleValue (nullable wrapper). nullopt -> SQL NULL.
  if (req->has_service_fee()) {
    p.append(req->service_fee().value());
  } else {
    p.append(nullptr);
  }
  // ChargerType: store as the SQL enum label.
  p.append(ChargerTypeLabel(req->charger_type()));
  p.append(req->source_category_id());
  // Location / Remark are empty -> SQL NULL.
  if (req->location().empty()) {
    p.append(nullptr);
  } else {
    p.append(req->location());
  }
  if (req->remark().empty()) {
    p.append(nullptr);
  } else {
    p.append(req->remark());
  }
  return p;
}
}  // namespace

grpc::Status ChargingServiceImpl::CreateCharging(
    grpc::ServerContext* ctx, const CreateChargingRequest* req,
    Charging* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ChargingService/CreateCharging";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  if (auto v = ValidateCharging(req); !v.ok()) {
    scope.set_status(v);
    return v;
  }

  try {
    auto id = NewUuid();
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto params = ChargeParams(id, req);
    db::Exec(tx,
        "INSERT INTO charging (Id, VehicleId, StartTime, EndTime, "
        "StartPercent, EndPercent, StartMileage, EndMileage, "
        "KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, ChargerType, "
        "SourceCategoryId, Location, Remark) VALUES "
        "($1, $2, $3, $4, $5, $6, $7, $8, $9, "
        "$10, $11, $12, $13::charger_type_enum, $14, $15, $16)",
        "ChargingService.CreateCharging",
        params);
    auto inserted = db::Exec(tx,
        "SELECT Id, VehicleId, StartTime::text, EndTime::text, "
        "StartPercent, EndPercent, StartMileage, EndMileage, "
        "KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, ChargerType, "
        "SourceCategoryId, Location, Remark FROM charging WHERE Id=$1",
        "ChargingService.CreateCharging",
        id);
    if (inserted.empty()) {
      auto s = grpc::Status(grpc::StatusCode::INTERNAL, "INSERT returned no row");
      scope.set_status(s);
      return s;
    }
    *resp = RowToCharging(inserted[0]);
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=CreateCharging vehicle_id={} charger_type={} reason={}",
        req->vehicle_id(), ChargerTypeLabel(req->charger_type()), e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status ChargingServiceImpl::GetCharging(
    grpc::ServerContext* ctx, const GetChargingRequest* req, Charging* resp) {
  static constexpr const char* kMethod = "/evgrpc.ChargingService/GetCharging";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = db::Exec(tx,
        "SELECT Id, VehicleId, StartTime::text, EndTime::text, "
        "StartPercent, EndPercent, StartMileage, EndMileage, "
        "KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, ChargerType, "
        "SourceCategoryId, Location, Remark FROM charging WHERE Id=$1",
        "ChargingService.GetCharging",
        req->id());
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "charging not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToCharging(result[0]);
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status ChargingServiceImpl::UpdateCharging(
    grpc::ServerContext* ctx, const UpdateChargingRequest* req, Charging* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ChargingService/UpdateCharging";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  // Re-use CreateChargingRequest-shaped view for validation
  // (same fields as UpdateChargingRequest minus `id`).
  CreateChargingRequest v;
  v.set_vehicle_id(req->vehicle_id());
  *v.mutable_start_time() = req->start_time();
  *v.mutable_end_time() = req->end_time();
  v.set_start_percent(req->start_percent());
  v.set_end_percent(req->end_percent());
  v.set_start_mileage_km(req->start_mileage_km());
  v.set_end_mileage_km(req->end_mileage_km());
  v.set_kwh_charged(req->kwh_charged());
  v.set_cost(req->cost());
  v.set_electricity_unit_price(req->electricity_unit_price());
  if (req->has_service_fee()) {
    v.mutable_service_fee()->set_value(req->service_fee().value());
  }
  v.set_charger_type(req->charger_type());
  v.set_source_category_id(req->source_category_id());
  v.set_location(req->location());
  v.set_remark(req->remark());
  if (auto s = ValidateCharging(&v); !s.ok()) {
    scope.set_status(s);
    return s;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    pqxx::params p;
    p.append(req->vehicle_id());
    p.append(TimestampString(req->start_time()));
    p.append(TimestampString(req->end_time()));
    p.append(req->start_percent());
    p.append(req->end_percent());
    p.append(req->start_mileage_km());
    p.append(req->end_mileage_km());
    p.append(req->kwh_charged());
    p.append(req->cost());
    p.append(req->electricity_unit_price());
    if (req->has_service_fee()) {
      p.append(req->service_fee().value());
    } else {
      p.append(nullptr);
    }
    p.append(ChargerTypeLabel(req->charger_type()));
    p.append(req->source_category_id());
    if (req->location().empty()) p.append(nullptr); else p.append(req->location());
    if (req->remark().empty())   p.append(nullptr); else p.append(req->remark());
    p.append(req->id());
    auto result = db::Exec(tx,
        "UPDATE charging SET VehicleId=$1, StartTime=$2, "
        "EndTime=$3, StartPercent=$4, EndPercent=$5, "
        "StartMileage=$6, EndMileage=$7, KwhCharged=$8, Cost=$9, "
        "ElectricityUnitPrice=$10, ServiceFee=$11, "
        "ChargerType=$12::charger_type_enum, SourceCategoryId=$13, "
        "Location=$14, Remark=$15 WHERE Id=$16 "
        "RETURNING Id, VehicleId, StartTime::text, EndTime::text, "
        "StartPercent, EndPercent, StartMileage, EndMileage, "
        "KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, ChargerType, "
        "SourceCategoryId, Location, Remark",
        "ChargingService.UpdateCharging",
        p);
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "charging not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToCharging(result[0]);
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status ChargingServiceImpl::DeleteCharging(
    grpc::ServerContext* ctx, const DeleteChargingRequest* req,
    google::protobuf::Empty* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ChargingService/DeleteCharging";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = db::Exec(tx,
        "DELETE FROM charging WHERE Id=$1", "ChargingService.DeleteCharging",
        req->id());
    if (result.affected_rows() == 0) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "charging not found");
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

grpc::Status ChargingServiceImpl::ListChargings(
    grpc::ServerContext* ctx, const ListChargingsRequest* req,
    ListChargingsResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.ChargingService/ListChargings";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    int page_size = req->page_size() > 0 ? req->page_size() : 50;

    std::string sql =
        "SELECT Id, VehicleId, StartTime::text, EndTime::text, "
        "StartPercent, EndPercent, StartMileage, EndMileage, "
        "KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, ChargerType, "
        "SourceCategoryId, Location, Remark FROM charging WHERE 1=1";
    pqxx::params p;

    if (!req->vehicle_id().empty()) {
      sql += " AND VehicleId = $" + std::to_string(p.size() + 1);
      p.append(req->vehicle_id());
    }
    if (req->has_start_after()) {
      sql += " AND StartTime > $" + std::to_string(p.size() + 1);
      p.append(TimestampString(req->start_after()));
    }
    if (req->has_start_before()) {
      sql += " AND StartTime < $" + std::to_string(p.size() + 1);
      p.append(TimestampString(req->start_before()));
    }
    if (req->charger_type() != ChargerType::CHARGER_TYPE_UNSPECIFIED) {
      sql += " AND ChargerType = $" + std::to_string(p.size() + 1) +
             "::charger_type_enum";
      p.append(ChargerTypeLabel(req->charger_type()));
    }
    if (!req->source_category_id().empty()) {
      sql += " AND SourceCategoryId = $" + std::to_string(p.size() + 1);
      p.append(req->source_category_id());
    }
    int offset = 0;
    if (auto s = ParsePageToken(req->page_token(), &offset); !s.ok()) {
      scope.set_status(s);
      return s;
    }
    sql += " ORDER BY StartTime DESC LIMIT $" +
           std::to_string(p.size() + 1) +
           " OFFSET $" + std::to_string(p.size() + 2);
    p.append(page_size + 1);
    p.append(offset);

    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = db::Exec(tx, sql, "ChargingService.ListChargings", p);
    bool has_more = result.size() > static_cast<size_t>(page_size);
    size_t emit = has_more ? static_cast<size_t>(page_size) : result.size();
    for (size_t i = 0; i < emit; ++i) {
      *resp->add_chargings() = RowToCharging(result[i]);
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