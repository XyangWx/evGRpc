#include <exception>
#include "config/config.h"
#include "log/log.h"

int main() {
  evgrpc::log::Init();
  try {
    auto c = evgrpc::Config::Load();
    auto server_log = evgrpc::log::Get("server");
    server_log->info("evGRpc starting on port {}", c.grpc_port);
    // Server wiring (gRPC ServerBuilder, service registration, signal
    // handling) lands in Task 15. Today the binary exits 0 after logging
    // the bound port — enough to prove the logging path end-to-end.
    return 0;
  } catch (const std::exception& e) {
    auto server_log = evgrpc::log::Get("server");
    server_log->error("config error: {}", e.what());
    return 1;
  }
}