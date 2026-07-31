#include "util/rpc_scope.h"

#include <iomanip>
#include <random>
#include <sstream>
#include "log/log.h"

namespace evgrpc {

namespace {

// 16 hex chars of randomness; unique enough for log correlation,
// not a real UUID (no version/variant bits). Avoids pulling libuuid
// into the hot path for every RPC. thread_local so per-thread rng
// doesn't contend.
std::string NewReqId() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  uint64_t a = rng();
  uint64_t b = rng();
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << a
     << std::setw(16) << std::setfill('0') << b;
  return os.str();
}

}  // namespace

RpcScope::RpcScope(const std::string& method,
                   const std::multimap<grpc::string_ref, grpc::string_ref>& md,
                   const std::string& subject)
    : method_(method),
      metadata_(md),
      subject_(subject.empty() ? std::string{"<unknown>"} : subject),
      req_id_(NewReqId()),
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