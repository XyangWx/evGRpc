#include "services/display_service.h"

#include <google/protobuf/util/time_util.h>
#include <pqxx/pqxx>
#include <string>
#include <unordered_map>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "services/charger/charger_type.h"
#include "util/rpc_scope.h"

namespace evgrpc {

namespace {

// proto Timestamp -> ISO 8601 string for binding to TIMESTAMP column.
std::string TimestampString(const google::protobuf::Timestamp& ts) {
  return google::protobuf::util::TimeUtil::ToString(ts);
}

// Returns nullopt when the proto Timestamp is unset (has_timestamp() ==
// false). libpqxx binds std::optional<std::string> as either the value
// or NULL — lets us write one query that handles "filter not set".
std::optional<std::string> MaybeTimestamp(
    const google::protobuf::Timestamp& ts) {
  if (ts.seconds() == 0 && ts.nanos() == 0) return std::nullopt;
  return TimestampString(ts);
}

}  // namespace

DisplayServiceImpl::DisplayServiceImpl(PgPool* pool, JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status DisplayServiceImpl::GetVehicleCostSummary(
    grpc::ServerContext* ctx, const GetVehicleCostSummaryRequest* req,
    VehicleCostSummary* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetVehicleCostSummary";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  if (req->vehicle_id().empty()) {
    auto s = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "vehicle_id is required");
    scope.set_status(s);
    return s;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto start_ts = MaybeTimestamp(req->start_time());
    auto end_ts = MaybeTimestamp(req->end_time());
    auto result = tx.exec_params(
        // Two CTEs: charging aggregates, consumption aggregates (for
        // total km). Combined in one SELECT to save a round-trip.
        "WITH c AS ("
        "  SELECT COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION AS total_cost, "
        "         COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh "
        "  FROM charging c "
        "  WHERE c.VehicleId = $1 "
        "    AND ($2::TIMESTAMP IS NULL OR c.StartTime >= $2) "
        "    AND ($3::TIMESTAMP IS NULL OR c.StartTime <= $3)"
        "), k AS ("
        "  SELECT COALESCE(SUM(end_mileage - begin_mileage), 0)::DOUBLE PRECISION AS total_km "
        "  FROM consumption "
        "  WHERE VehicleId = $1 "
        "    AND ($2::TIMESTAMP IS NULL OR \"Start\" >= $2) "
        "    AND ($3::TIMESTAMP IS NULL OR \"Start\" <= $3)"
        ") "
        "SELECT c.total_cost, c.total_kwh, k.total_km FROM c, k",
        req->vehicle_id(), start_ts, end_ts);

    if (result.empty()) {
      // Both CTEs return one row each (via the COALESCE-on-empty-set
      // trick); the SELECT then has exactly one row. Empty here means
      // something went wrong.
      auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
      scope.set_status(s);
      return s;
    }
    const auto& row = result[0];
    const double total_cost = row["total_cost"].as<double>();
    const double total_kwh = row["total_kwh"].as<double>();
    const double total_km = row["total_km"].as<double>();

    resp->set_vehicle_id(req->vehicle_id());
    resp->set_total_cost(total_cost);
    resp->set_total_kwh(total_kwh);
    resp->set_avg_yuan_per_kwh(total_kwh > 0 ? total_cost / total_kwh : 0.0);
    resp->set_avg_yuan_per_km(total_km > 0 ? total_cost / total_km : 0.0);
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=GetVehicleCostSummary vehicle_id={} reason={}",
        req->vehicle_id(), e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetMonthlyReport(
    grpc::ServerContext* ctx, const GetMonthlyReportRequest* req,
    PeriodReport* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetMonthlyReport";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  if (req->year() < 1900 || req->month() < 1 || req->month() > 12) {
    auto s = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "year must be >= 1900 and month must be 1..12");
    scope.set_status(s);
    return s;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    // Single-query aggregation: SUM(Cost), SUM(KwhCharged), plus the
    // SUM of (end - start) mileage from the consumption table for the
    // matching month. Optional vehicle filter via WHERE VehicleId=$N.
    // We can't reuse the WITH-CTE form here because the two source tables
    // have different date columns (charging.StartTime vs
    // consumption."Start"). Two subqueries combined is clearer than one
    // big CTE.
    pqxx::params p;
    p.append(req->year());
    p.append(req->month());
    if (!req->vehicle_id().empty()) p.append(req->vehicle_id());

    std::string sql =
        "SELECT "
        "  COALESCE((SELECT SUM(c.Cost) FROM charging c "
        "            WHERE EXTRACT(YEAR FROM c.StartTime) = $1 "
        "              AND EXTRACT(MONTH FROM c.StartTime) = $2"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND c.VehicleId = $3"))
        + "), 0)::DOUBLE PRECISION AS total_cost, "
        "  COALESCE((SELECT SUM(c.KwhCharged) FROM charging c "
        "            WHERE EXTRACT(YEAR FROM c.StartTime) = $1 "
        "              AND EXTRACT(MONTH FROM c.StartTime) = $2"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND c.VehicleId = $3"))
        + "), 0)::DOUBLE PRECISION AS total_kwh, "
        "  COALESCE((SELECT SUM(end_mileage - begin_mileage) FROM consumption "
        "            WHERE EXTRACT(YEAR FROM \"Start\") = $1 "
        "              AND EXTRACT(MONTH FROM \"Start\") = $2"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND VehicleId = $3"))
        + "), 0)::DOUBLE PRECISION AS total_km";
    auto result = tx.exec_params(sql, p);
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
      scope.set_status(s);
      return s;
    }
    const auto& row = result[0];
    resp->set_year(req->year());
    resp->set_month(req->month());
    resp->set_total_cost(row["total_cost"].as<double>());
    resp->set_total_kwh(row["total_kwh"].as<double>());
    resp->set_total_km(row["total_km"].as<double>());
    if (!req->vehicle_id().empty()) resp->set_vehicle_id(req->vehicle_id());
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetAnnualReport(
    grpc::ServerContext* ctx, const GetAnnualReportRequest* req,
    PeriodReport* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetAnnualReport";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  if (req->year() < 1900) {
    auto s = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "year must be >= 1900");
    scope.set_status(s);
    return s;
  }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    pqxx::params p;
    p.append(req->year());
    if (!req->vehicle_id().empty()) p.append(req->vehicle_id());

    // For annual reports: month=0 per spec ("0 = annual; 1-12 = monthly").
    // Note: month=0 is invalid for EXTRACT(MONTH FROM ...) (which returns
    // 1..12), so we deliberately only filter on year here.
    std::string sql =
        "SELECT "
        "  COALESCE((SELECT SUM(c.Cost) FROM charging c "
        "            WHERE EXTRACT(YEAR FROM c.StartTime) = $1"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND c.VehicleId = $2"))
        + "), 0)::DOUBLE PRECISION AS total_cost, "
        "  COALESCE((SELECT SUM(c.KwhCharged) FROM charging c "
        "            WHERE EXTRACT(YEAR FROM c.StartTime) = $1"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND c.VehicleId = $2"))
        + "), 0)::DOUBLE PRECISION AS total_kwh, "
        "  COALESCE((SELECT SUM(end_mileage - begin_mileage) FROM consumption "
        "            WHERE EXTRACT(YEAR FROM \"Start\") = $1"
        + (req->vehicle_id().empty() ? std::string{}
                                     : std::string(" AND VehicleId = $2"))
        + "), 0)::DOUBLE PRECISION AS total_km";
    auto result = tx.exec_params(sql, p);
    if (result.empty()) {
      auto s = grpc::Status(grpc::StatusCode::INTERNAL, "no aggregate row");
      scope.set_status(s);
      return s;
    }
    const auto& row = result[0];
    resp->set_year(req->year());
    resp->set_month(0);  // 0 = annual (per spec)
    resp->set_total_cost(row["total_cost"].as<double>());
    resp->set_total_kwh(row["total_kwh"].as<double>());
    resp->set_total_km(row["total_km"].as<double>());
    if (!req->vehicle_id().empty()) resp->set_vehicle_id(req->vehicle_id());
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetCostByChargerType(
    grpc::ServerContext* ctx, const GetCostByChargerTypeRequest* req,
    GetCostByChargerTypeResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetCostByChargerType";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto start_ts = MaybeTimestamp(req->start_time());
    auto end_ts = MaybeTimestamp(req->end_time());

    // GROUP BY ChargerType. CAST to CHARGER_TYPE_ENUM is a no-op
    // identity (it's already typed), but the SUMs need DOUBLE PRECISION
    // casts so the proto doubles resolve.
    auto result = tx.exec_params(
        "SELECT ChargerType, "
        "       COALESCE(SUM(Cost), 0)::DOUBLE PRECISION AS total_cost, "
        "       COALESCE(SUM(KwhCharged), 0)::DOUBLE PRECISION AS total_kwh "
        "FROM charging "
        "WHERE ($1::TEXT IS NULL OR VehicleId = $1) "
        "  AND ($2::TIMESTAMP IS NULL OR StartTime >= $2) "
        "  AND ($3::TIMESTAMP IS NULL OR StartTime <= $3) "
        "GROUP BY ChargerType "
        "ORDER BY ChargerType",
        // Optional vehicle_id: bind std::nullopt when unset so SQL
        // gets NULL and the `($1::TEXT IS NULL OR VehicleId = $1)`
        // predicate is satisfied (matches all vehicles).
        req->vehicle_id().empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{req->vehicle_id()},
        start_ts,
        end_ts);

    for (const auto& row : result) {
      auto* b = resp->add_breakdowns();
      b->set_charger_type(ChargerTypeFromLabel(row["ChargerType"].as<std::string>()));
      b->set_total_cost(row["total_cost"].as<double>());
      b->set_total_kwh(row["total_kwh"].as<double>());
      const double total_kwh = row["total_kwh"].as<double>();
      b->set_avg_yuan_per_kwh(total_kwh > 0
                                  ? row["total_cost"].as<double>() / total_kwh
                                  : 0.0);
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=GetCostByChargerType vehicle_id={} reason={}",
        req->vehicle_id(), e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetCostBySourceCategory(
    grpc::ServerContext* ctx, const GetCostBySourceCategoryRequest* req,
    GetCostBySourceCategoryResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetCostBySourceCategory";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto start_ts = MaybeTimestamp(req->start_time());
    auto end_ts = MaybeTimestamp(req->end_time());

    // JOIN charging ⨝ source_category to get the category name.
    // GROUP BY source_category so each row = one breakdown.
    auto result = tx.exec_params(
        "SELECT c.SourceCategoryId, sc.Name AS SourceCategoryName, "
        "       COALESCE(SUM(c.Cost), 0)::DOUBLE PRECISION AS total_cost, "
        "       COALESCE(SUM(c.KwhCharged), 0)::DOUBLE PRECISION AS total_kwh "
        "FROM charging c "
        "JOIN source_category sc ON sc.Id = c.SourceCategoryId "
        "WHERE ($1::TEXT IS NULL OR c.VehicleId = $1) "
        "  AND ($2::TIMESTAMP IS NULL OR c.StartTime >= $2) "
        "  AND ($3::TIMESTAMP IS NULL OR c.StartTime <= $3) "
        "GROUP BY c.SourceCategoryId, sc.Name "
        "ORDER BY total_cost DESC",
        req->vehicle_id().empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{req->vehicle_id()},
        start_ts,
        end_ts);

    for (const auto& row : result) {
      auto* b = resp->add_breakdowns();
      b->set_source_category_id(row["SourceCategoryId"].as<std::string>());
      b->set_source_category_name(row["SourceCategoryName"].as<std::string>());
      b->set_total_cost(row["total_cost"].as<double>());
      b->set_total_kwh(row["total_kwh"].as<double>());
      const double total_kwh = row["total_kwh"].as<double>();
      b->set_avg_yuan_per_kwh(total_kwh > 0
                                  ? row["total_cost"].as<double>() / total_kwh
                                  : 0.0);
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=GetCostBySourceCategory vehicle_id={} reason={}",
        req->vehicle_id(), e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetConsumptionEfficiency(
    grpc::ServerContext* ctx, const GetConsumptionEfficiencyRequest* req,
    GetConsumptionEfficiencyResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetConsumptionEfficiency";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  // The `consumption` table doesn't store kWh directly — kWh lives in
  // the `charging` table. We derive consumption efficiency by aggregating
  // the two tables independently under the same (vehicle_id, time
  // window) predicate, then joining in C++ on vehicle_id. The two
  // SUMs use the SAME time-window filter so the values are
  // commensurable (a charging event and a consumption event in the
  // same hour share that hour's energy and mileage).
  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto start_ts = MaybeTimestamp(req->start_time());
    auto end_ts = MaybeTimestamp(req->end_time());

    // Per-vehicle totals from consumption.
    auto km_rows = tx.exec_params(
        "SELECT VehicleId, "
        "       COALESCE(SUM(end_mileage - begin_mileage), 0)::DOUBLE PRECISION AS total_km "
        "FROM consumption "
        "WHERE ($1::TEXT IS NULL OR VehicleId = $1) "
        "  AND ($2::TIMESTAMP IS NULL OR \"Start\" >= $2) "
        "  AND ($3::TIMESTAMP IS NULL OR \"Start\" <= $3) "
        "GROUP BY VehicleId",
        req->vehicle_id().empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{req->vehicle_id()},
        start_ts, end_ts);

    // Per-vehicle totals from charging.
    auto kwh_rows = tx.exec_params(
        "SELECT VehicleId, "
        "       COALESCE(SUM(KwhCharged), 0)::DOUBLE PRECISION AS total_kwh "
        "FROM charging "
        "WHERE ($1::TEXT IS NULL OR VehicleId = $1) "
        "  AND ($2::TIMESTAMP IS NULL OR StartTime >= $2) "
        "  AND ($3::TIMESTAMP IS NULL OR StartTime <= $3) "
        "GROUP BY VehicleId",
        req->vehicle_id().empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{req->vehicle_id()},
        start_ts, end_ts);

    // Merge by VehicleId. C++-side join is fine for two small result
    // sets (one row per vehicle per table). If the per-vehicle counts
    // ever grow into the thousands, push this into SQL.
    std::unordered_map<std::string, double> kwh_by_vehicle;
    for (const auto& row : kwh_rows) {
      kwh_by_vehicle.emplace(row["VehicleId"].as<std::string>(),
                             row["total_kwh"].as<double>());
    }
    for (const auto& row : km_rows) {
      const auto vid = row["VehicleId"].as<std::string>();
      const double total_km = row["total_km"].as<double>();
      const double total_kwh = kwh_by_vehicle.count(vid)
                                  ? kwh_by_vehicle.at(vid) : 0.0;
      auto* e = resp->add_efficiencies();
      e->set_vehicle_id(vid);
      e->set_total_km(total_km);
      e->set_total_kwh(total_kwh);
      e->set_km_per_kwh(total_kwh > 0 ? total_km / total_kwh : 0.0);
      // kWh/100km is the inverse of km/kWh, scaled to 100km.
      e->set_kwh_per_100km(total_km > 0 ? (total_kwh / total_km) * 100.0 : 0.0);
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=GetConsumptionEfficiency reason={}", e.what());
    scope.set_status(s);
    return s;
  }
}

grpc::Status DisplayServiceImpl::GetRangeAccuracy(
    grpc::ServerContext* ctx, const GetRangeAccuracyRequest* req,
    GetRangeAccuracyResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.DisplayService/GetRangeAccuracy";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto start_ts = MaybeTimestamp(req->start_time());
    auto end_ts = MaybeTimestamp(req->end_time());

    // Dashboard range: SUM(StartRange - EndRange) over charging
    // (per spec: "dashboard_range_total_km" = sum of ranges the car
    // THOUGHT it had at charge-start minus what it had at charge-end
    // — i.e. range lost during the charging event).
    // Actual mileage: SUM(EndMileage - BeginMileage) over consumption
    // (km actually driven between consumption events).
    // Accuracy ratio = actual / dashboard — values < 1 mean the car
    // over-reported its range; values > 1 mean it under-reported.
    auto rows = tx.exec_params(
        "SELECT d.VehicleId, "
        "       COALESCE(d.dashboard_range, 0)::DOUBLE PRECISION AS dashboard, "
        "       COALESCE(m.actual_mileage, 0)::DOUBLE PRECISION AS actual "
        "FROM ("
        "  SELECT VehicleId, SUM(StartRange - EndRange) AS dashboard_range "
        "  FROM charging "
        "  WHERE ($1::TEXT IS NULL OR VehicleId = $1) "
        "    AND ($2::TIMESTAMP IS NULL OR StartTime >= $2) "
        "    AND ($3::TIMESTAMP IS NULL OR StartTime <= $3) "
        "  GROUP BY VehicleId"
        ") d FULL OUTER JOIN ("
        "  SELECT VehicleId, SUM(EndMileage - BeginMileage) AS actual_mileage "
        "  FROM consumption "
        "  WHERE ($1::TEXT IS NULL OR VehicleId = $1) "
        "    AND ($2::TIMESTAMP IS NULL OR \"Start\" >= $2) "
        "    AND ($3::TIMESTAMP IS NULL OR \"Start\" <= $3) "
        "  GROUP BY VehicleId"
        ") m ON d.VehicleId = m.VehicleId",
        req->vehicle_id().empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{req->vehicle_id()},
        start_ts, end_ts);

    for (const auto& row : rows) {
      // FULL OUTER JOIN can yield null VehicleId; coalesce to empty.
      const std::string vid = row["VehicleId"].is_null()
                                  ? std::string{}
                                  : row["VehicleId"].as<std::string>();
      if (vid.empty()) continue;
      const double dashboard = row["dashboard"].as<double>();
      const double actual = row["actual"].as<double>();
      auto* a_out = resp->add_accuracies();
      a_out->set_vehicle_id(vid);
      a_out->set_dashboard_range_total_km(dashboard);
      a_out->set_actual_mileage_total_km(actual);
      a_out->set_accuracy_ratio(dashboard > 0 ? actual / dashboard : 0.0);
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    evgrpc::log::Get("db")->warn(
        "method=GetRangeAccuracy reason={}", e.what());
    scope.set_status(s);
    return s;
  }
}

}  // namespace evgrpc