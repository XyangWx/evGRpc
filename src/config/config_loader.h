#pragma once
#include <string>

namespace evgrpc {

// Section structs — defaults match spec §2.3.
struct DatabaseConfig {
    std::string url;  // postgresql://...
};
struct OAuthConfig {
    std::string issuer_url = "";
    std::string audience = "";
    int jwks_cache_ttl_seconds = 3600;
    // OIDC discovery HTTP fetch timeouts (seconds). Defaults are tuned for
    // cold-start https discovery where the first DNS+TLS round-trip can
    // exceed a few seconds.
    int discovery_connect_timeout_seconds = 5;
    int discovery_read_timeout_seconds = 15;
};
struct GrpcConfig {
    int port = 50051;
};
struct LogConfig {
    std::string level = "info";  // trace|debug|info|warn|error|critical
    std::string file = "";       // empty = no file sink
    int max_size_mb = 100;
    int max_files = 7;
};

struct SchemaConfig {
    DatabaseConfig database;
    OAuthConfig oauth;
    GrpcConfig grpc;
    LogConfig log;
};

// Read <path>, parse JSON, validate every field, return SchemaConfig.
// Throws std::runtime_error whose .what() joins all validation errors
// with '\n' (so the caller can write it directly to stderr).
SchemaConfig LoadSchema(const std::string& path);

}  // namespace evgrpc
