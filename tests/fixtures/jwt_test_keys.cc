// Test fixture helper: 2048-bit RSA keypair generation (OpenSSL 3.x
// provider/keymgmt API) and RS256 JWT signing (jwt-cpp).
//
// Real implementation replaces the brief's placeholder `return {};`. The
// RSA keypair is generated via EVP_PKEY_CTX + EVP_PKEY_keygen (OpenSSL 3.0+
// provider API; legacy RSA_generate_key_ex is deprecated and produces
// warnings under -DOPENSSL_API_COMPAT=3.0).
#include "jwt_test_keys.h"

#include <jwt-cpp/jwt.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace evgrpc::test {
namespace {

// RAII wrappers for OpenSSL resources — destructor ensures no leaks
// even when an intermediate call throws.
struct EvpPKeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept {
    if (p) EVP_PKEY_free(p);
  }
};
using EvpPKeyPtr = std::unique_ptr<EVP_PKEY, EvpPKeyDeleter>;

struct EvpPkeyCtxDeleter {
  void operator()(EVP_PKEY_CTX* c) const noexcept {
    if (c) EVP_PKEY_CTX_free(c);
  }
};
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;

struct BioDeleter {
  void operator()(BIO* b) const noexcept {
    if (b) BIO_free(b);
  }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// Drains a memory BIO into a std::string. The BIO_get_mem_data macro
// writes the internal buffer pointer into `out_ptr` (no copy).
std::string BioToString(BIO* bio) {
  char* buf = nullptr;
  const long n = BIO_get_mem_data(bio, &buf);
  if (n <= 0) throw std::runtime_error("BIO_get_mem_data returned no data");
  return std::string(buf, static_cast<size_t>(n));
}

// Generates a 2048-bit RSA EVP_PKEY using the OpenSSL 3.0 provider API.
// Provider names: "RSA" resolved via the default provider loaded by
// OPENSSL_init_crypto() at startup; no explicit OSSL_LIB_CTX needed.
EvpPKeyPtr GenerateRsa2048() {
  EvpPkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
  if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new_from_name(\"RSA\") failed");

  if (EVP_PKEY_keygen_init(ctx.get()) <= 0)
    throw std::runtime_error("EVP_PKEY_keygen_init failed");

  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0)
    throw std::runtime_error("EVP_PKEY_CTX_set_rsa_keygen_bits(2048) failed");

  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0)
    throw std::runtime_error("EVP_PKEY_keygen failed");

  return EvpPKeyPtr{raw};
}

}  // namespace

RsaKeyPair GenerateRsaKeyPair(const std::string& kid) {
  EvpPKeyPtr pkey = GenerateRsa2048();

  // Private key: unencrypted PKCS#8 ("-----BEGIN PRIVATE KEY-----"). The
  // plain PEM_write_bio_PrivateKey 2-arg overload was removed in OpenSSL
  // 3.0; PEM_write_bio_PKCS8PrivateKey writes the same format jwt-cpp
  // expects (verified against build/_deps/jwt_cpp-src/example/rsa-create.cpp).
  BioPtr priv_bio{BIO_new(BIO_s_mem())};
  if (!priv_bio) throw std::runtime_error("BIO_new(BIO_s_mem) for private key failed");
  if (PEM_write_bio_PKCS8PrivateKey(priv_bio.get(), pkey.get(),
                                    /*enc=*/nullptr, /*kstr=*/nullptr, /*klen=*/0,
                                    /*cb=*/nullptr, /*u=*/nullptr) != 1) {
    throw std::runtime_error("PEM_write_bio_PKCS8PrivateKey failed");
  }
  std::string pem_private = BioToString(priv_bio.get());

  // Public key: SubjectPublicKeyInfo ("-----BEGIN PUBLIC KEY-----").
  // Uses the non-deprecated _ex variant: PEM_write_bio_PUBKEY_ex(bio,
  // pkey, libctx=nullptr, propq=nullptr).
  BioPtr pub_bio{BIO_new(BIO_s_mem())};
  if (!pub_bio) throw std::runtime_error("BIO_new(BIO_s_mem) for public key failed");
  if (PEM_write_bio_PUBKEY_ex(pub_bio.get(), pkey.get(), /*libctx=*/nullptr,
                              /*propq=*/nullptr) != 1) {
    throw std::runtime_error("PEM_write_bio_PUBKEY_ex failed");
  }
  std::string pem_public = BioToString(pub_bio.get());

  return RsaKeyPair{std::move(pem_private), std::move(pem_public), kid};
}

std::string SignJwt(const RsaKeyPair& key, const std::string& issuer,
                    const std::string& audience, int64_t exp_offset_seconds) {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer(issuer)
      .set_audience(audience)
      .set_subject("test-user")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::seconds(exp_offset_seconds))
      .set_header_claim("kid", jwt::claim(std::string(key.kid)))
      .sign(jwt::algorithm::rs256(key.pem_public, key.pem_private, "", ""));
}

}  // namespace evgrpc::test
