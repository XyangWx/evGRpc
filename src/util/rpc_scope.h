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
//   grpc::Status CreateVehicle(grpc::ServerContext* ctx, ...) override {
//     RpcScope scope("/evgrpc.VehicleService/CreateVehicle",
//                    ctx->client_metadata(), /*subject=*/"");
//     auto auth = evgrpc::Authenticate(scope.metadata(), *validator_);
//     if (!auth.ok()) { scope.set_status(auth); return auth; }
//     // ... business logic
//     scope.set_status(some_status);
//     return scope.status();
//   }
//
// Logs:
//   [entry]  service.info("req_id={} method={} subject={}", ...)
//   [exit]   service.info("req_id={} method={} status={} latency_ms={}", ...)
class RpcScope {
 public:
  RpcScope(const std::string& method,
           const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
           const std::string& subject);
  ~RpcScope();

  RpcScope(const RpcScope&) = delete;
  RpcScope& operator=(const RpcScope&) = delete;

  // Final status to log on destruction. Defaults to OK.
  void set_status(grpc::Status s) { status_ = s; }
  const grpc::Status& status() const { return status_; }

  // Re-exposed metadata so the body can pass it to Authenticate().
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