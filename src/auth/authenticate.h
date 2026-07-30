#pragma once
#include <grpcpp/support/status.h>
#include <grpcpp/support/string_ref.h>
#include <map>
#include "auth/jwt_validator.h"

namespace evgrpc {

// Per-RPC bearer-token authentication helper.
//
// Each generated service method calls this as its first action:
//   auto status = evgrpc::Authenticate(ctx->client_metadata(), *validator_);
//   if (!status.ok()) return status;
//
// `client_metadata` is the initial-metadata multimap from the gRPC call
// (typically `grpc::ServerContext::client_metadata()`). Taking it as a
// reference (not a `ServerContext*`) makes this helper trivially
// unit-testable without constructing a gRPC server.
//
// Returns:
//   - `grpc::Status::OK` if `Authorization: Bearer <token>` is present
//     and the token passes JWT validation (RS256, iss, aud, exp).
//   - `grpc::Status(UNAUTHENTICATED, "<reason>")` on any failure:
//     missing header, non-Bearer scheme, malformed token, signature
//     mismatch, expired token, unknown `kid`, wrong issuer/audience.
//
// All validation is fail-closed; the helper never throws.
grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator);

}  // namespace evgrpc
