#include "services/vehicle_service.h"

#include <google/protobuf/util/time_util.h>
#include <pqxx/pqxx>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "db/exec.h"
#include "util/page_token_parse.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

namespace {

// Map a SQL row to the Vehicle proto. purchase_date is read as
// `PurchaseDate::text` (ISO 8601 date string) so we can parse it
// into google.protobuf.Timestamp without a tz-naive ambiguity.
//
// protobuf 4.x renamed BuildFromString -> FromString and the latter
// returns `bool` (out-param Timestamp*) instead of absl::StatusOr.
Vehicle RowToVehicle(const pqxx::row& r) {
  Vehicle v;
  v.set_id(r["Id"].as<std::string>());
  v.set_brand(r["Brand"].as<std::string>());
  v.set_calibrated_range_km(r["CalibratedRange"].as<int32_t>());
  v.set_battery_capacity_kwh(r["BatteryCapacity"].as<double>());
  const auto date_str = r["PurchaseDate"].as<std::string>();
  if (!date_str.empty()) {
    google::protobuf::Timestamp ts;
    if (google::protobuf::util::TimeUtil::FromString(
            date_str + "T00:00:00Z", &ts)) {
      *v.mutable_purchase_date() = ts;
    }
  }
  v.set_license_plate(r["LicensePlate"].as<std::string>());
  return v;
}

// Extract just the first 10 chars (YYYY-MM-DD) from a Timestamp's
// string form, so we can bind to a `date` column without libpqxx
// needing a Timestamp parser.
std::string TimestampDateString(const google::protobuf::Timestamp& ts) {
  return google::protobuf::util::TimeUtil::ToString(ts).substr(0, 10);
}

}  // namespace

VehicleServiceImpl::VehicleServiceImpl(PgPool* pool, JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status VehicleServiceImpl::CreateVehicle(
    grpc::ServerContext* ctx, const CreateVehicleRequest* req, Vehicle* resp) {
  static constexpr const char* kMethod = "/evgrpc.VehicleService/CreateVehicle";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    db::Exec(tx,
        "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate, LicensePlate) VALUES ($1, $2, $3, $4, $5::date, $6)",
        "VehicleService.CreateVehicle",
        id,
        req->brand(),
        req->calibrated_range_km(),
        req->battery_capacity_kwh(),
        TimestampDateString(req->purchase_date()),
        req->license_plate());
    tx.commit();

    // Read it back so the response reflects whatever the table did
    // (defaults, triggers, generated columns).
    GetVehicleRequest get_req;
    get_req.set_id(id);
    auto read = GetVehicle(ctx, &get_req, resp);
    if (!read.ok()) { scope.set_status(read); return read; }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status VehicleServiceImpl::GetVehicle(
    grpc::ServerContext* ctx, const GetVehicleRequest* req, Vehicle* resp) {
  static constexpr const char* kMethod = "/evgrpc.VehicleService/GetVehicle";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = db::Exec(tx,
        "SELECT Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate FROM vehicle WHERE Id = $1",
        "VehicleService.GetVehicle",
        req->id());
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "vehicle not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToVehicle(result[0]);
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status VehicleServiceImpl::UpdateVehicle(
    grpc::ServerContext* ctx, const UpdateVehicleRequest* req, Vehicle* resp) {
  static constexpr const char* kMethod = "/evgrpc.VehicleService/UpdateVehicle";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = db::Exec(tx,
        "UPDATE vehicle SET Brand=$2, CalibratedRange=$3, BatteryCapacity=$4, "
        "PurchaseDate=$5::date, LicensePlate=$6 WHERE Id=$1 "
        "RETURNING Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate",
        "VehicleService.UpdateVehicle",
        req->id(),
        req->brand(),
        req->calibrated_range_km(),
        req->battery_capacity_kwh(),
        TimestampDateString(req->purchase_date()),
        req->license_plate());
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "vehicle not found");
      scope.set_status(s);
      return s;
    }
    *resp = RowToVehicle(result[0]);
    tx.commit();
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status VehicleServiceImpl::DeleteVehicle(
    grpc::ServerContext* ctx, const DeleteVehicleRequest* req,
    google::protobuf::Empty* resp) {
  static constexpr const char* kMethod = "/evgrpc.VehicleService/DeleteVehicle";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = db::Exec(tx,
        "DELETE FROM vehicle WHERE Id=$1",
        "VehicleService.DeleteVehicle",
        req->id());
    if (result.affected_rows() == 0) {
      auto s = grpc::Status(grpc::StatusCode::NOT_FOUND, "vehicle not found");
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

grpc::Status VehicleServiceImpl::ListVehicles(
    grpc::ServerContext* ctx, const ListVehiclesRequest* req,
    ListVehiclesResponse* resp) {
  static constexpr const char* kMethod = "/evgrpc.VehicleService/ListVehicles";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    // Cap page_size at kMaxPageSize-1 so the +1 (used to detect
    // "has next page") doesn't overflow into negative territory, which
    // PG rejects with "LIMIT must not be negative". Also prevents users
    // from accidentally requesting huge pages.
    constexpr int kMaxPageSize = 1000;
    int page_size = req->page_size() > 0
                       ? std::min(req->page_size(), kMaxPageSize - 1)
                       : 50;
    int offset = 0;
    if (auto s = ParsePageToken(req->page_token(), &offset); !s.ok()) {
      scope.set_status(s);
      return s;
    }
    // Fetch page_size+1 rows so we can tell "has next page" without
    // a separate COUNT(*) round-trip.
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = db::Exec(tx,
        "SELECT Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate FROM vehicle "
        "ORDER BY PurchaseDate DESC, Id LIMIT $1 OFFSET $2",
        "VehicleService.ListVehicles",
        page_size + 1, offset);
    bool has_more = result.size() > static_cast<size_t>(page_size);
    size_t emit = has_more ? static_cast<size_t>(page_size) : result.size();
    for (size_t i = 0; i < emit; ++i) {
      *resp->add_vehicles() = RowToVehicle(result[i]);
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