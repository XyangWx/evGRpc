#pragma once
#include <functional>
#include <optional>
#include <string>

namespace evgrpc {

// Subset of validated JWT claims returned by JwtValidator::Validate.
// `subject` is the OIDC `sub` claim (used for audit logging).
struct Claims {
  std::string subject;
  std::string issuer;
  std::string audience;
};

// RS256 bearer-token validator. The validator is fail-closed: any error
// during decoding, key resolution, signature verification, or claim checks
// returns std::nullopt rather than throwing or returning a partial result.
//
// `resolve_key` maps a JWT header `kid` to its PEM-encoded public key
// (SubjectPublicKeyInfo, "-----BEGIN PUBLIC KEY-----"). The JWKS cache in
// Task 8 provides this callback; tests provide an in-memory map.
struct JwtValidator {
  std::string issuer;
  std::string audience;
  std::function<std::optional<std::string>(const std::string& kid)> resolve_key;

  std::optional<Claims> Validate(const std::string& token) const;
};

}  // namespace evgrpc
