#pragma once
#include <chrono>
#include <map>
#include <string>
#include <grpcpp/support/status.h>
#include <grpcpp/support/string_ref.h>

namespace evgrpc {

// RAII helper that logs RPC entry (on construction) and exit (on destruction)
// via the `service` named logger, per spec §5.6.
//
// Usage in a service method body:
//   const auto req_id = evgrpc::NewReqId();
//   Claims claims;
//   auto auth = evgrpc::Authenticate(scope.metadata(), *validator_, &claims);
//   evgrpc::log::Get("auth")->info(
//       "method=... subject={} reason={} req_id={}", ...);
//   RpcScope scope("/evgrpc.X/Y", ctx->client_metadata(),
//                  auth.ok() ? claims.subject : "<unknown>", req_id);
//   if (!auth.ok()) { scope.set_status(auth); return auth; }
//   // ... business logic
//   scope.set_status(s);
//   return scope.status();
//
// Constructor params:
//   `method`   — fully-qualified RPC name (e.g. "/evgrpc.VehicleService/CreateVehicle")
//   `metadata` — initial-metadata multimap from the gRPC call (typically
//                `ctx->client_metadata()`); re-exposed via `.metadata()` so
//                the body can pass it to `Authenticate()`.
//   `subject`  — JWT `sub` claim on auth pass; `<unknown>` on auth fail.
//   `req_id`   — correlation id; if empty, RpcScope generates one
//                internally. Pass an explicit one (via `NewReqId()`) when
//                the auth-outcome log line needs to share the id.
//
// Logs:
//   [entry]  service.info("req_id={} method={} subject={}", ...)
//   [exit]   service.info("req_id={} method={} status={} latency_ms={}", ...)
class RpcScope {
 public:
  RpcScope(const std::string& method,
           const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
           const std::string& subject,
           const std::string& req_id = "");
  ~RpcScope();

  RpcScope(const RpcScope&) = delete;
  RpcScope& operator=(const RpcScope&) = delete;

  // Final status to log on destruction. Defaults to OK.
  void set_status(grpc::Status s) { status_ = s; }
  const grpc::Status& status() const { return status_; }

  const std::multimap<grpc::string_ref, grpc::string_ref>& metadata() const {
    return metadata_;
  }

  const std::string& method() const { return method_; }
  const std::string& subject() const { return subject_; }
  const std::string& req_id() const { return req_id_; }

 private:
  std::string method_;
  std::multimap<grpc::string_ref, grpc::string_ref> metadata_;
  std::string subject_;
  std::string req_id_;
  grpc::Status status_ = grpc::Status::OK;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace evgrpc