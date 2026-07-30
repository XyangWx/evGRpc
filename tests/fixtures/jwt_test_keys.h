#pragma once
#include <cstdint>
#include <string>

namespace evgrpc::test {

// A 2048-bit RSA keypair serialized as PEM strings plus a key id.
struct RsaKeyPair {
    std::string pem_private;  // PKCS#8: "-----BEGIN PRIVATE KEY-----"
    std::string pem_public;   // SubjectPublicKeyInfo: "-----BEGIN PUBLIC KEY-----"
    std::string kid;
};

// Generates a fresh 2048-bit RSA keypair in memory, encodes both PEM strings,
// and returns them tagged with `kid`.
//
// This is intentionally slow (RSA keygen takes ~100 ms on a modern CPU);
// tests should cache the result at fixture scope, not call it per-test.
RsaKeyPair GenerateRsaKeyPair(const std::string& kid);

// Signs an RS256 JWT with `key.pem_private` using jwt-cpp. The token's
// header carries `kid` (as `set_header_claim("kid", ...)`) and its payload
// has `iss`, `aud`, `sub="test-user"`, `iat=now`, `exp=now+exp_offset_seconds`.
// `exp_offset_seconds` may be negative to produce an already-expired token.
std::string SignJwt(const RsaKeyPair& key, const std::string& issuer,
                    const std::string& audience, int64_t exp_offset_seconds);

}  // namespace evgrpc::test
