#include "services/source_category_service.h"

#include <pqxx/pqxx>
#include "auth/authenticate_rpc.h"
#include "db/error.h"
#include "util/rpc_scope.h"
#include "util/uuid.h"

namespace evgrpc {

SourceCategoryServiceImpl::SourceCategoryServiceImpl(PgPool* pool,
                                                     JwtValidator* validator)
    : pool_(pool), validator_(validator) {}

grpc::Status SourceCategoryServiceImpl::CreateSourceCategory(
    grpc::ServerContext* ctx, const CreateSourceCategoryRequest* req,
    SourceCategory* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.SourceCategoryService/CreateSourceCategory";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    auto conn = pool_->acquire();
    pqxx::work tx(*conn);
    auto id = NewUuid();
    tx.exec_params(
        "INSERT INTO source_category (Id, Name) VALUES ($1, $2)",
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

grpc::Status SourceCategoryServiceImpl::SearchSourceCategory(
    grpc::ServerContext* ctx, const SearchSourceCategoryRequest* req,
    SearchSourceCategoryResponse* resp) {
  static constexpr const char* kMethod =
      "/evgrpc.SourceCategoryService/SearchSourceCategory";
  const auto a = AuthenticateRpc(ctx, *validator_, kMethod);
  RpcScope scope(kMethod, ctx->client_metadata(), a.subject, a.req_id);
  if (!a.status.ok()) { scope.set_status(a.status); return a.status; }

  try {
    int limit = req->limit() > 0 ? req->limit() : 50;

    auto conn = pool_->acquire();
    pqxx::nontransaction tx(*conn);
    auto result = tx.exec_params(
        "SELECT Id, Name FROM source_category WHERE Name ^@ $1 "
        "ORDER BY Name LIMIT $2",
        req->prefix(), limit);

    for (const auto& row : result) {
      auto* s = resp->add_matches();
      s->set_id(row["Id"].as<std::string>());
      s->set_name(row["Name"].as<std::string>());
    }
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    auto s = ToGrpcStatus(e);
    scope.set_status(s);
    return s;
  }
}

}  // namespace evgrpc