#include "util/rpc_scope.h"

#include "log/log.h"
#include "util/req_id.h"

namespace evgrpc {

RpcScope::RpcScope(const std::string& method,
                   const std::multimap<grpc::string_ref, grpc::string_ref>& md,
                   const std::string& subject,
                   const std::string& req_id)
    : method_(method),
      metadata_(md),
      subject_(subject.empty() ? std::string{"<unknown>"} : subject),
      req_id_(req_id.empty() ? NewReqId() : req_id),
      start_(std::chrono::steady_clock::now()) {
  evgrpc::log::Get("service")->info(
      "req_id={} method={} subject={}", req_id_, method_, subject_);
}

RpcScope::~RpcScope() {
  auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_).count();
  evgrpc::log::Get("service")->info(
      "req_id={} method={} status={} latency_ms={}",
      req_id_, method_,
      static_cast<int>(status_.error_code()),
      latency_ms);
}

}  // namespace evgrpc