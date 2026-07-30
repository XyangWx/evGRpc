#include "auth/jwt_validator.h"

#include <jwt-cpp/jwt.h>

#include <exception>

namespace evgrpc {

std::optional<Claims> JwtValidator::Validate(const std::string& token) const {
  try {
    // 1. Decode (no verification yet) so we can pull `kid` from the header
    //    and resolve the public key. jwt-cpp throws on malformed input.
    auto decoded = jwt::decode(token);

    // 2. Resolve the public key. The header `kid` is optional; treat
    //    absent kid as a non-match (the cache lookup will return nullopt).
    std::string kid;
    if (decoded.has_key_id()) kid = decoded.get_key_id();
    auto pem = resolve_key(kid);
    if (!pem) return std::nullopt;

    // 3. Build the verifier. Configure only RS256 (algorithm allow-list)
    //    and the iss/aud constraints. jwt-cpp's default leeway is 0;
    //    that's what we want (Task 8's clock-skew adjustment, if needed,
    //    lives in the cache layer).
    auto verifier = jwt::verify()
                        .allow_algorithm(jwt::algorithm::rs256(*pem, "", "", ""))
                        .with_issuer(issuer)
                        .with_audience(audience);

    // 4. Verify. Throws on signature mismatch, expiry, nbf violation, or
    //    iss/aud mismatch. exp/nbf/iss/aud defaults of `leeway = 0` apply.
    verifier.verify(decoded);

    // 5. Extract the small set of claims callers care about.
    Claims c;
    c.subject = decoded.has_subject() ? decoded.get_subject() : "";
    c.issuer  = decoded.has_issuer()  ? decoded.get_issuer()  : "";
    // jwt-cpp's get_audience() returns a set<string> because the `aud`
    // claim can be either a single string or an array. Pick the first
    // entry (the validator above already confirmed this set contains our
    // expected audience).
    if (decoded.has_audience()) {
      auto aud_set = decoded.get_audience();
      c.audience = aud_set.empty() ? std::string{} : *aud_set.begin();
    }
    return c;
  } catch (const std::exception&) {
    // Fail-closed: any exception (decoding failure, signature failure,
    // expired token, wrong issuer/audience, missing claim) becomes an
    // empty optional. The error type is intentionally swallowed so the
    // API does not leak *why* the token was rejected.
    return std::nullopt;
  }
}

}  // namespace evgrpc
