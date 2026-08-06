#pragma once
#include <chrono>
#include <string>

namespace evgrpc::auth {

struct OidcDiscoveryConfig {
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(2)};
    std::chrono::milliseconds read_timeout{std::chrono::seconds(5)};
};

// GET {issuer_url}/.well-known/openid-configuration, parse JSON,
// return discovery["jwks_uri"].
//
// Throws std::runtime_error on:
//   - issuer_url not starting with "http://" or "https://"
//   - non-2xx HTTP response
//   - connect or read timeout
//   - response body not valid JSON
//   - missing or non-string 'jwks_uri' field
//   - jwks_uri not starting with "http://" or "https://"
//
// URL construction: trim trailing '/' from issuer_url, then append
// "/.well-known/openid-configuration".
std::string DiscoverJwksUri(const std::string& issuer_url,
                            const OidcDiscoveryConfig& cfg = {});

}  // namespace evgrpc::auth
