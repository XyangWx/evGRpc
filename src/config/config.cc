#include "config/config.h"
#include <cstdlib>
#include <string>

namespace evgrpc {

namespace {
std::string require_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) throw std::runtime_error(std::string("missing env var: ") + name);
    return v;
}
std::string opt_env(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}
int opt_env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return std::stoi(v);
}
}  // namespace

Config Config::Load() {
    Config c;
    c.database_url        = require_env("DATABASE_URL");
    c.oauth_issuer_url    = require_env("OAUTH_ISSUER_URL");
    c.oauth_audience      = require_env("OAUTH_AUDIENCE");
    c.oauth_jwks_url      = require_env("OAUTH_JWKS_URL");
    c.oauth_jwks_cache_ttl_seconds = opt_env_int("OAUTH_JWKS_CACHE_TTL", 3600);
    c.grpc_port           = opt_env_int("GRPC_PORT", 50051);
    return c;
}

}  // namespace evgrpc
