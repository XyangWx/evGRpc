#include "config/config.h"
#include "auth/oidc_discovery.h"
#include <stdexcept>

namespace evgrpc {

RuntimeConfig LoadConfig(const std::string& path) {
    SchemaConfig schema = LoadSchema(path);  // throws on bad schema

    RuntimeConfig out;
    out.database = schema.database;
    out.oauth.issuer_url = schema.oauth.issuer_url;
    out.oauth.audience = schema.oauth.audience;
    out.oauth.jwks_cache_ttl_seconds = schema.oauth.jwks_cache_ttl_seconds;
    out.grpc = schema.grpc;
    out.log = schema.log;

    // OIDC discovery — populates jwks_url. Throws on failure.
    out.oauth.jwks_url =
        auth::DiscoverJwksUri(schema.oauth.issuer_url);

    return out;
}

}  // namespace evgrpc
