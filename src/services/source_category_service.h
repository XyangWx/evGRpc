#pragma once
#include <grpcpp/grpcpp.h>
#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/source_category.pb.h"
#include "evgrpc/source_category.grpc.pb.h"

namespace evgrpc {

// Concrete implementation of the generated `SourceCategoryService::Service`
// from `proto/evgrpc/source_category.proto`. Mirror of Task 11's
// WeatherService — same 2-RPC shape (Create + Search), same
// autocomplete semantics via PostgreSQL's `^@` operator.
//
// Backed by `PgPool` (Task 4) for storage and `JwtValidator` (Task 7)
// for auth. Uses the Task 10.5 `AuthenticateRpc` helper so auth + service
// log lines share a single `req_id`.
class SourceCategoryServiceImpl final : public SourceCategoryService::Service {
 public:
  SourceCategoryServiceImpl(PgPool* pool, JwtValidator* validator);

  grpc::Status CreateSourceCategory(grpc::ServerContext*,
                                    const CreateSourceCategoryRequest*,
                                    SourceCategory*) override;
  grpc::Status SearchSourceCategory(grpc::ServerContext*,
                                     const SearchSourceCategoryRequest*,
                                     SearchSourceCategoryResponse*) override;

 private:
  PgPool* pool_;
  JwtValidator* validator_;
};

}  // namespace evgrpc