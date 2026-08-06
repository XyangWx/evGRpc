#pragma once
#include "config/config_loader.h"
#include <string>

namespace evgrpc {

// Fully-resolved config (after OIDC discovery has populated jwks_url).
// This is what main.cc threads through PgPool, JwksCache, JwtValidator,
// log::Init, and ServerBuilder.
struct RuntimeConfig {
    DatabaseConfig database;
    struct {
        std::string issuer_url;
        std::string jwks_url;     // populated by OIDC discovery
        std::string audience;
        int jwks_cache_ttl_seconds;
    } oauth;
    GrpcConfig grpc;
    LogConfig log;
};

// Combined: LoadSchema(path) + DiscoverJwksUri(issuer_url) + assemble
// RuntimeConfig. Throws std::runtime_error on any failure (file read,
// JSON parse, schema validation, OIDC discovery HTTP/JSON/field).
RuntimeConfig LoadConfig(const std::string& path);

}  // namespace evgrpc
