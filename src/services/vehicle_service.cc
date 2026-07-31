#include "services/vehicle_service.h"

#include <google/protobuf/util/time_util.h>
#include <pqxx/pqxx>
#include "auth/authenticate.h"
#include "db/error.h"
#include "log/log.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

namespace {

// Map a SQL row to the Vehicle proto. purchase_date is read as
// `PurchaseDate::text` (ISO 8601 date string) so we can parse it
// into google.protobuf.Timestamp without a tz-naive ambiguity.
Vehicle RowToVehicle(const pqxx::row& r) {
  Vehicle v;
  v.set_id(r["Id"].as<std::string>());
  v.set_brand(r["Brand"].as<std::string>());
  v.set_calibrated_range_km(r["CalibratedRange"].as<int32_t>());
  v.set_battery_capacity_kwh(r["BatteryCapacity"].as<double>());
  const auto date_str = r["PurchaseDate"].as<std::string>();
  if (!date_str.empty()) {
    auto ts = google::protobuf::util::TimeUtil::BuildFromString(
        date_str + "T00:00:00Z");
    if (ts.ok()) {
      *v.mutable_purchase_date() = ts.value();
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

// Authenticate helper that ALSO returns the subject on success.
// Today's `evgrpc::Authenticate()` discards claims. Until that's
// refactored (see task-10-report.md TODO), we accept subject="" in
// RpcScope and let the `auth` logger capture the auth outcome separately.
//
// Wait — we DON'T want to log auth twice (here + in Authenticate).
// For Task 10 we ship subject="" and accept the deferred TODO.
// The `auth` logger's pass/fail reason IS logged by the implementer
// of Authenticate() in a follow-up; for now neither logs auth.

grpc::Status VehicleServiceImpl::CreateVehicle(
    grpc::ServerContext* ctx, const CreateVehicleRequest* req, Vehicle* resp) {
  RpcScope scope("/evgrpc.VehicleService/CreateVehicle",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    tx.exec_params(
        "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate, LicensePlate) VALUES ($1, $2, $3, $4, $5::date, $6)",
        id,
        req->brand(),
        req->calibrated_range_km(),
        req->battery_capacity_kwh(),
        TimestampDateString(req->purchase_date()),
        req->license_plate());
    tx.commit();

    // Read it back so the response reflects whatever the table did
    // (defaults, triggers, generated columns).
    auto read = GetVehicle(ctx, &GetVehicleRequest{id}, resp);
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
  RpcScope scope("/evgrpc.VehicleService/GetVehicle",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = tx.exec_params(
        "SELECT Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate FROM vehicle WHERE Id = $1",
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
  RpcScope scope("/evgrpc.VehicleService/UpdateVehicle",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = tx.exec_params(
        "UPDATE vehicle SET Brand=$2, CalibratedRange=$3, BatteryCapacity=$4, "
        "PurchaseDate=$5::date, LicensePlate=$6 WHERE Id=$1 "
        "RETURNING Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate",
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
  RpcScope scope("/evgrpc.VehicleService/DeleteVehicle",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto result = tx.exec_params(
        "DELETE FROM vehicle WHERE Id=$1", req->id());
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
  RpcScope scope("/evgrpc.VehicleService/ListVehicles",
                 ctx->client_metadata(), /*subject=*/"");

  auto auth = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
  if (!auth.ok()) { scope.set_status(auth); return auth; }

  try {
    auto page_size = req->page_size() > 0 ? req->page_size() : 50;
    int offset = 0;
    if (!req->page_token().empty()) {
      offset = std::stoi(req->page_token());
      if (offset < 0) offset = 0;
    }
    // Fetch page_size+1 rows so we can tell "has next page" without
    // a separate COUNT(*) round-trip.
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = tx.exec_params(
        "SELECT Id, Brand, CalibratedRange, BatteryCapacity, "
        "PurchaseDate::text, LicensePlate FROM vehicle "
        "ORDER BY PurchaseDate DESC, Id LIMIT $1 OFFSET $2",
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