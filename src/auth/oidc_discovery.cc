#include "auth/oidc_discovery.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace evgrpc::auth {

namespace {

bool IsHttpUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

std::string BuildDiscoveryUrl(const std::string& issuer_url) {
    std::string base = issuer_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/.well-known/openid-configuration";
}

// Parse "http://host[:port]" into host + port. Returns port 80 for
// http and 443 for https if not specified. Throws on non-http(s).
struct HostPort { std::string host; int port; };

HostPort SplitIssuer(const std::string& issuer_url) {
    if (!IsHttpUrl(issuer_url)) {
        throw std::runtime_error(
            "oidc discovery: issuer_url must be http(s):// (got \"" +
            issuer_url + "\")");
    }
    bool https = issuer_url.rfind("https://", 0) == 0;
    std::string rest = issuer_url.substr(https ? 8 : 7);  // strip scheme

    auto slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    auto colon = hostport.find(':');
    HostPort hp;
    if (colon == std::string::npos) {
        hp.host = hostport;
        hp.port = https ? 443 : 80;
    } else {
        hp.host = hostport.substr(0, colon);
        hp.port = std::stoi(hostport.substr(colon + 1));
    }
    return hp;
}

}  // namespace

std::string DiscoverJwksUri(const std::string& issuer_url,
                            const OidcDiscoveryConfig& cfg) {
    auto hp = SplitIssuer(issuer_url);  // throws on non-http
    std::string url = BuildDiscoveryUrl(issuer_url);

    httplib::Client client(hp.host, hp.port);
    client.set_connection_timeout(cfg.connect_timeout);
    client.set_read_timeout(cfg.read_timeout);
    // cpp_httplib supports http only on this code path; the public
    // OIDC discovery URL the test points at is http. For https
    // production issuers, set up an http -> https proxy or use
    // libcurl in a follow-up. (Documented in spec §1 Out of Scope.)

    // cpp_httplib::Client::Get treats its argument as the request path,
    // not a full URL — passing the full URL produces a malformed request
    // line ("GET http://host/.well-known/...") that the server rejects
    // with 404. Pass only the path component ("/.well-known/...").
    auto scheme_pos = url.find("://");
    auto path_start = (scheme_pos == std::string::npos)
                          ? std::string::npos
                          : url.find('/', scheme_pos + 3);
    std::string path_only =
        (path_start == std::string::npos) ? "/" : url.substr(path_start);

    auto res = client.Get(path_only.c_str());
    if (!res) {
        throw std::runtime_error(
            "oidc discovery: HTTP request failed for " + url);
    }
    if (res->status / 100 != 2) {
        throw std::runtime_error(
            "oidc discovery: non-2xx response " +
            std::to_string(res->status) + " from " + url);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(res->body);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            "oidc discovery: malformed JSON body: " +
            std::string(e.what()));
    }

    if (!j.is_object() || !j.contains("jwks_uri") ||
        !j["jwks_uri"].is_string()) {
        throw std::runtime_error(
            "oidc discovery: response missing string 'jwks_uri' field");
    }

    std::string jwks = j["jwks_uri"].get<std::string>();
    if (!IsHttpUrl(jwks)) {
        throw std::runtime_error(
            "oidc discovery: jwks_uri must be http(s):// (got \"" +
            jwks + "\")");
    }
    return jwks;
}

}  // namespace evgrpc::auth
