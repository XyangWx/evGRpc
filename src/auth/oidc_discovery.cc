#include "auth/oidc_discovery.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <memory>
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

// RAII wrapper so the CURL* handle is freed on every exit path.
struct CurlDeleter {
    void operator()(CURL* c) const noexcept {
        if (c) curl_easy_cleanup(c);
    }
};
using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// Fetches `url` via libcurl and returns the body. Throws on transport
// failure or non-2xx. libcurl handles both http and https (unlike the
// previous cpp-httplib path, which was http-only — this is what makes
// https issuers like auth.mksword.com work).
std::string FetchDiscovery(const std::string& url,
                           const OidcDiscoveryConfig& cfg) {
    CurlPtr curl{curl_easy_init()};
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(cfg.connect_timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(cfg.read_timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl_easy_perform failed: ") +
                                 curl_easy_strerror(rc));
    }

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("oidc discovery: non-2xx response " +
                                 std::to_string(http_code) + " from " + url);
    }
    return body;
}

}  // namespace

std::string DiscoverJwksUri(const std::string& issuer_url,
                            const OidcDiscoveryConfig& cfg) {
    if (!IsHttpUrl(issuer_url)) {
        throw std::runtime_error(
            "oidc discovery: issuer_url must be http(s):// (got \"" +
            issuer_url + "\")");
    }
    const std::string url = BuildDiscoveryUrl(issuer_url);

    const std::string body = FetchDiscovery(url, cfg);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
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
