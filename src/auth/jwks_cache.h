#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace evgrpc {

// Thread-safe JWKS (JSON Web Key Set) cache for OAuth 2.0 / OIDC token
// verification. Fetches JWKS JSON from `url` lazily on first miss and on
// TTL expiry, parses RSA keys (kty=RSA) into PEM-encoded
// SubjectPublicKeyInfo, and serves them by `kid` lookup.
//
// The validator (JwtValidator) does not own the cache: it holds a
// `resolve_key(kid) -> optional<pem>` callback that wraps
// `JwksCache::GetKey`. The cache is fail-closed — any HTTP, parse, or
// JWK conversion failure results in GetKey returning std::nullopt rather
// than throwing.
class JwksCache {
 public:
  // `url` is the JWKS endpoint (e.g., https://idp.example.com/.well-known/jwks.json).
  // `ttl` is the cache lifetime; after expiry the next GetKey call refreshes.
  JwksCache(std::string url, std::chrono::seconds ttl);

  JwksCache(const JwksCache&) = delete;
  JwksCache& operator=(const JwksCache&) = delete;

  // Returns the PEM-encoded public key (SubjectPublicKeyInfo,
  // "-----BEGIN PUBLIC KEY-----") for the given `kid`, or std::nullopt on
  // any failure (network, parse, unknown kid, JWK-to-PEM conversion).
  //
  // Concurrent calls are serialized via an internal mutex; the actual
  // HTTP fetch is single-flight per call site (the mutex guarantees only
  // one refresh is in progress at a time per JwksCache instance).
  std::optional<std::string> GetKey(const std::string& kid);

 private:
  // Fetches the JWKS JSON, parses it, and atomically replaces `keys_`.
  // Must be called with `mu_` held.
  void refresh();

  std::string url_;
  std::chrono::seconds ttl_;
  std::mutex mu_;
  std::unordered_map<std::string, std::string> keys_;  // kid -> PEM
  std::chrono::steady_clock::time_point fetched_at_{};
};

}  // namespace evgrpc