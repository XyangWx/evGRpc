#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "auth/jwt_validator.h"
#include "auth/jwks_cache.h"
#include "config/config.h"
#include "db/pool.h"
#include "log/log.h"
#include "services/charging_service.h"
#include "services/consumption_service.h"
#include "services/display_service.h"
#include "services/source_category_service.h"
#include "services/vehicle_service.h"
#include "services/weather_service.h"

namespace {

// Shutdown signal flag set by the SIGINT / SIGTERM handler. std::atomic<>
// gives us signal-safe read/write; the handler must do nothing else
// besides setting this flag. Server->Shutdown() is called from the main
// thread after Wait() observes the flag (see below).
std::atomic<bool> g_shutdown_requested{false};

extern "C" void HandleSignal(int /*signum*/) {
  g_shutdown_requested.store(true, std::memory_order_release);
}

// Background thread: poll the flag and trigger server->Shutdown() when set.
// (Polling, not condition-variable-wait, because sigwait + pthread condvar
// is platform-dependent and we want this to work the same on Linux + macOS.)
void InstallShutdownHook(grpc::Server* server) {
  std::thread([server]() {
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    auto deadline = std::chrono::system_clock::now() +
                    std::chrono::seconds(5);
    server->Shutdown(deadline);
  }).detach();
}

}  // namespace

int main() {
  evgrpc::log::Init();
  try {
    auto cfg = evgrpc::Config::Load();
    auto server_log = evgrpc::log::Get("server");

    // 1. Storage layer (PostgreSQL connection pool).
    evgrpc::PgPool pool(cfg.database_url);

    // 2. Auth: JWKS cache → JwtValidator (each Validate() call asks the
    //    cache for the public key by kid; cache refreshes on miss).
    auto jwks = std::make_shared<evgrpc::JwksCache>(
        cfg.oauth_jwks_url,
        std::chrono::seconds(cfg.oauth_jwks_cache_ttl_seconds));
    auto validator = std::make_shared<evgrpc::JwtValidator>(
        evgrpc::JwtValidator{
            .issuer = cfg.oauth_issuer_url,
            .audience = cfg.oauth_audience,
            .resolve_key =
                [jwks](const std::string& kid) {
                  return jwks->GetKey(kid);
                },
        });

    // 3. Service implementations (5 of 6 — DisplayService lands in
    //    Tasks 16–19 and gets registered here once it exists).
    evgrpc::VehicleServiceImpl vehicle_svc(&pool, validator.get());
    evgrpc::WeatherServiceImpl weather_svc(&pool, validator.get());
    evgrpc::SourceCategoryServiceImpl sc_svc(&pool, validator.get());
    evgrpc::ConsumptionServiceImpl consumption_svc(&pool, validator.get());
    evgrpc::ChargingServiceImpl charging_svc(&pool, validator.get());
    evgrpc::DisplayServiceImpl display_svc(&pool, validator.get());

    // 4. Build & start the gRPC server.
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(cfg.grpc_port),
                              grpc::InsecureServerCredentials());
    builder.RegisterService(&vehicle_svc);
    builder.RegisterService(&weather_svc);
    builder.RegisterService(&sc_svc);
    builder.RegisterService(&consumption_svc);
    builder.RegisterService(&charging_svc);
    builder.RegisterService(&display_svc);
    auto server = builder.BuildAndStart();
    if (!server) {
      server_log->critical("failed to bind :{} (port in use?)",
                            cfg.grpc_port);
      return 1;
    }

    // 5. Signal handling: SIGINT / SIGTERM → server->Shutdown(5s).
    //    std::signal is signal-safe enough for our use (just sets an
    //    atomic flag); the actual Shutdown call happens in a polling
    //    thread so the handler stays minimal.
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    InstallShutdownHook(server.get());

    server_log->info("evGRpc listening on :{} (5 services registered; "
                      "SIGINT/SIGTERM → graceful shutdown within 5s)",
                      cfg.grpc_port);
    server->Wait();
    server_log->info("evGRpc shutdown complete");
    return 0;
  } catch (const std::exception& e) {
    auto server_log = evgrpc::log::Get("server");
    server_log->critical("fatal: {}", e.what());
    return 1;
  }
}