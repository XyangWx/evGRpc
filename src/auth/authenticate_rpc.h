#pragma once
#include <grpcpp/server_context.h>
#include <string>
#include "auth/authenticate.h"
#include "auth/jwt_validator.h"
#include "log/log.h"
#include "util/req_id.h"

namespace evgrpc {

// Per-RPC auth prologue. Generates a correlation id, validates the bearer
// token via `Authenticate`, logs the auth outcome (spec §5.6 audit line),
// and returns the pieces the service method needs to construct an
// `RpcScope` with a matching req_id and the real subject (or
// `<unknown>` on failure).
//
// Used as the first three lines of every service method body:
//   const auto a = AuthenticateRpc(ctx, *validator_, method_name);
//   RpcScope scope(method_name, ctx->client_metadata(), a.subject, a.req_id);
//   if (!a.status.ok()) { scope.set_status(a.status); return a.status; }
//   // ... business logic
struct AuthenticateRpcResult {
  std::string req_id;      // 32-hex correlation id; share with RpcScope
  std::string subject;     // claims.subject on success; "<unknown>" on failure
  std::string reason;      // "ok" / "missing_header" / "non_bearer" / "bad_signature"
  grpc::Status status;     // OK or UNAUTHENTICATED
};

inline AuthenticateRpcResult AuthenticateRpc(
    grpc::ServerContext* ctx,
    const JwtValidator& validator,
    const std::string& method) {
  AuthenticateRpcResult r;
  r.req_id = NewReqId();
  Claims claims;
  r.status = evgrpc::Authenticate(ctx->client_metadata(), validator,
                                  &claims, &r.reason);
  r.subject = r.status.ok() ? claims.subject : std::string{"<unknown>"};

  // spec §5.6 auth-outcome log format: `method=<X> subject=<Y>
  // reason=<category> req_id=<uuid>`. One line per RPC, on both pass
  // and fail — there must be a "no auth" paper trail for every
  // rejected request.
  log::Get("auth")->info(
      "method={} subject={} reason={} req_id={}",
      method, r.subject, r.reason, r.req_id);

  return r;
}

}  // namespace evgrpc