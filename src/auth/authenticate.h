#pragma once
#include <grpcpp/support/status.h>
#include <grpcpp/support/string_ref.h>
#include <map>
#include <string>
#include "auth/jwt_validator.h"

namespace evgrpc {

// Per-RPC bearer-token authentication helper.
//
// Each generated service method calls this as its first action:
//   Claims claims;
//   std::string reason;
//   auto status = evgrpc::Authenticate(ctx->client_metadata(), *validator_,
//                                      &claims, &reason);
//   log::Get("auth")->info(
//       "method=... subject={} reason={} req_id={}", method,
//       status.ok() ? claims.subject : std::string{"<unknown>"}, reason, req_id);
//   RpcScope scope(method, ctx->client_metadata(),
//                  status.ok() ? claims.subject : std::string{"<unknown>"},
//                  req_id);
//   if (!status.ok()) { scope.set_status(status); return status; }
//   // ... business logic
//
// `client_metadata` is the initial-metadata multimap from the gRPC call
// (typically `grpc::ServerContext::client_metadata()`). Taking it as a
// reference (not a `ServerContext*`) makes this helper trivially
// unit-testable without constructing a gRPC server.
//
// `out_claims` (optional) is filled with the validated JWT claims on
// success — `subject` (sub), `issuer` (iss), `audience` (aud). Pass
// `nullptr` if you only care about pass/fail.
//
// `out_reason` (optional) is filled with a short category string for
// pass/fail logging per spec §5.6:
//   - "ok"
//   - "missing_header"
//   - "non_bearer"
//   - "bad_signature"     (catch-all for malformed / expired / wrong
//                          iss / wrong aud / unknown kid; JwtValidator
//                          swallows the exception type today so finer
//                          granularity isn't possible without a
//                          JwtValidator API change)
//
// Returns:
//   - `grpc::Status::OK` if `Authorization: Bearer <token>` is present
//     and the token passes JWT validation (RS256, iss, aud, exp).
//   - `grpc::Status(UNAUTHENTICATED, "<human reason>")` on any failure.
//
// All validation is fail-closed; the helper never throws.
grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator,
    Claims* out_claims = nullptr,
    std::string* out_reason = nullptr);

}  // namespace evgrpc