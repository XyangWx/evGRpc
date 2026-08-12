#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
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
#include "util/args.h"

namespace {

constexpr char kUsage[] =
    "usage: evgrpc [--config <path>|-c <path>] [--help|-h]\n";

std::atomic<bool> g_shutdown_requested{false};

extern "C" void HandleSignal(int /*signum*/) {
  g_shutdown_requested.store(true, std::memory_order_release);
}

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

int main(int argc, char** argv) {
  evgrpc::ArgvResult args;
  try {
    args = evgrpc::ParseArgs(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "[evgrpc-args] " << e.what() << "\n" << kUsage;
    return 1;
  }
  if (args.help_requested) {
    std::cout << kUsage;
    return 0;
  }

  evgrpc::log::InitDefaults();

  evgrpc::RuntimeConfig cfg;
  try {
    cfg = evgrpc::LoadConfig(args.config_path);
  } catch (const std::exception& e) {
    auto server_log = evgrpc::log::Get("server");
    server_log->critical("config load failed: {}", e.what());
    return 1;
  }

  try {
    evgrpc::log::Init(cfg.log);
  } catch (const std::exception& e) {
    std::cerr << "[evgrpc-log] " << e.what() << std::endl;
    return 1;
  }
  auto server_log = evgrpc::log::Get("server");
  server_log->info("config loaded from {}", args.config_path);

  try {
    evgrpc::PgPool pool(cfg.database.url);

    auto jwks = std::make_shared<evgrpc::JwksCache>(
        cfg.oauth.jwks_url,
        std::chrono::seconds(cfg.oauth.jwks_cache_ttl_seconds));
    auto validator = std::make_shared<evgrpc::JwtValidator>(
        evgrpc::JwtValidator{
            .issuer = cfg.oauth.issuer_url,
            .audience = cfg.oauth.audience,
            .resolve_key =
                [jwks](const std::string& kid) {
                  return jwks->GetKey(kid);
                },
        });

    evgrpc::VehicleServiceImpl vehicle_svc(&pool, validator.get());
    evgrpc::WeatherServiceImpl weather_svc(&pool, validator.get());
    evgrpc::SourceCategoryServiceImpl sc_svc(&pool, validator.get());
    evgrpc::ConsumptionServiceImpl consumption_svc(&pool, validator.get());
    evgrpc::ChargingServiceImpl charging_svc(&pool, validator.get());
    evgrpc::DisplayServiceImpl display_svc(&pool, validator.get());

    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(cfg.grpc.port),
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
                            cfg.grpc.port);
      return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    InstallShutdownHook(server.get());

    server_log->info("evGRpc listening on :{} (6 services registered; "
                      "SIGINT/SIGTERM → graceful shutdown within 5s)",
                      cfg.grpc.port);
    server->Wait();
    server_log->info("evGRpc shutdown complete");
    return 0;
  } catch (const std::exception& e) {
    server_log->critical("fatal: {}", e.what());
    return 1;
  }
}
