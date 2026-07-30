#pragma once
#include <string>
#include <stdexcept>

namespace evgrpc {

struct Config {
    std::string database_url;
    std::string oauth_issuer_url;
    std::string oauth_audience;
    std::string oauth_jwks_url;
    int oauth_jwks_cache_ttl_seconds = 3600;
    int grpc_port = 50051;

    static Config Load();
};

}  // namespace evgrpc
