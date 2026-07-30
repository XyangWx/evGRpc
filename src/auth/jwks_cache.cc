#include "auth/jwks_cache.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace evgrpc {

namespace {

// --- RAII wrappers ---------------------------------------------------------
//
// OpenSSL resource lifetimes in this file:
//   - CURL*       : per-fetch handle, freed after curl_easy_perform
//   - BIGNUM*     : modulus + exponent from base64url JWK, fed to BLD
//   - OSSL_PARAM_BLD* : accumulates (key, BIGNUM*) pairs for serialization
//   - OSSL_PARAM* : owned array from BLD_to_param (freed with OSSL_PARAM_free)
//   - EVP_PKEY_CTX*: per-call key construction context
//   - EVP_PKEY*   : RSA public key constructed via EVP_PKEY_fromdata,
//                   serialized via PEM_write_bio_PUBKEY
//   - BIO*        : mem sink for PEM serialization
//
// The brief's pseudocode returns "" from jwk_to_pem; this file replaces
// that with a real implementation using OpenSSL 3.0+ APIs
// (EVP_PKEY_fromdata + OSSL_PARAM_BLD — the non-deprecated path; the
// legacy RSA_new/RSA_set0_key/EVP_PKEY_assign_RSA chain works too but
// emits -Wdeprecated-declarations warnings under OpenSSL 3.0).
struct CurlDeleter {
  void operator()(CURL* c) const noexcept {
    if (c) curl_easy_cleanup(c);
  }
};
using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;

struct BignumDeleter {
  void operator()(BIGNUM* b) const noexcept {
    if (b) BN_free(b);
  }
};
using BignumPtr = std::unique_ptr<BIGNUM, BignumDeleter>;

struct OsslParamBldDeleter {
  void operator()(OSSL_PARAM_BLD* b) const noexcept {
    if (b) OSSL_PARAM_BLD_free(b);
  }
};
using OSSL_PARAM_BLD_PTR = std::unique_ptr<OSSL_PARAM_BLD, OsslParamBldDeleter>;

struct EvpPkeyCtxDeleter {
  void operator()(EVP_PKEY_CTX* c) const noexcept {
    if (c) EVP_PKEY_CTX_free(c);
  }
};
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;

struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept {
    if (p) EVP_PKEY_free(p);
  }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct BioDeleter {
  void operator()(BIO* b) const noexcept {
    if (b) BIO_free(b);
  }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// --- HTTP fetch via libcurl ------------------------------------------------

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

// Fetches `url` via libcurl and returns the response body. Throws
// std::runtime_error on transport failure or non-2xx HTTP response. The
// file:// scheme (used by tests) succeeds with CURLINFO_RESPONSE_CODE = 0,
// so we allow both 2xx and 0.
std::string FetchUrl(const std::string& url) {
  CurlPtr curl{curl_easy_init()};
  if (!curl) throw std::runtime_error("curl_easy_init failed");

  std::string body;
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

  CURLcode rc = curl_easy_perform(curl.get());
  if (rc != CURLE_OK) {
    throw std::runtime_error(std::string("curl_easy_perform failed: ") +
                             curl_easy_strerror(rc));
  }
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  // file:// returns 0; HTTP returns the status code. Only HTTP non-2xx
  // is an error.
  if (http_code != 0 && (http_code < 200 || http_code >= 300)) {
    throw std::runtime_error("HTTP " + std::to_string(http_code) +
                             " fetching JWKS");
  }
  return body;
}

// --- base64url decode (RFC 7515 §3) ----------------------------------------
//
// JWKS uses base64url WITHOUT padding (RFC 7515 §2: "Base64url encoding
// ... does not include any '=', '?' or other characters used for padding").
// OpenSSL's BIO_f_base64 only handles STANDARD base64 (with '+/' and
// padding), so we rewrite the URL-safe alphabet ('-' -> '+', '_' -> '/')
// and re-add '=' padding to a multiple of 4.

std::string Base64UrlDecode(const std::string& in) {
  std::string b64;
  b64.reserve(in.size() + 4);
  for (char c : in) {
    if (c == '-') {
      b64.push_back('+');
    } else if (c == '_') {
      b64.push_back('/');
    } else {
      b64.push_back(c);
    }
  }
  while (b64.size() % 4 != 0) b64.push_back('=');

  // Chain: [base64 filter] -> [mem source of b64]. BIO_f_base64 reads
  // until EOF; BIO_FLAGS_BASE64_NO_NL suppresses newline handling (our
  // input has no newlines, but the flag also lets it accept a stream that
  // happens not to end on a newline).
  BioPtr filter{BIO_new(BIO_f_base64())};
  if (!filter) throw std::runtime_error("BIO_new(BIO_f_base64) failed");
  BIO_set_flags(filter.get(), BIO_FLAGS_BASE64_NO_NL);

  BioPtr source{BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()))};
  if (!source) throw std::runtime_error("BIO_new_mem_buf failed");

  BIO* chain = BIO_push(filter.release(), source.release());
  BioPtr chain_ptr{chain};

  std::string out;
  out.resize(b64.size());  // upper bound; decoded is <= encoded length
  int n = BIO_read(chain_ptr.get(), out.data(), static_cast<int>(out.size()));
  if (n < 0) throw std::runtime_error("base64url BIO_read failed");
  out.resize(static_cast<size_t>(n));
  return out;
}

// --- JWK → PEM (RFC 7518 §6.3.1) ------------------------------------------
//
// JWK for an RSA public key carries:
//   - n : base64url-encoded unsigned big-endian integer (modulus)
//   - e : base64url-encoded unsigned big-endian integer (public exponent)
//
// We decode both into raw big-endian bytes, build BIGNUMs, hand them to
// OSSL_PARAM_BLD (which serializes them in OpenSSL's NATIVE byte order
// for OSSL_PARAM_get_BN to read back), and serialize the resulting
// EVP_PKEY as PEM SubjectPublicKeyInfo via PEM_write_bio_PUBKEY. The
// output is the same format jwt-cpp's RS256 verifier expects
// ("-----BEGIN PUBLIC KEY-----").
//
// IMPORTANT: do NOT use OSSL_PARAM_construct_BN here. It tags the
// buffer with data_type=OSSL_PARAM_UNSIGNED_INTEGER but takes raw
// caller-owned bytes; the OpenSSL 3.0 OSSL_PARAM_get_BN receiver uses
// BN_native2bn which reads in NATIVE byte order (little-endian on
// x86_64). Passing big-endian BN_bn2bin output silently produces a
// truncated modulus. OSSL_PARAM_BLD_push_BN takes a BIGNUM* and does
// the BN_bn2nativepad conversion internally — the only correct path.

std::string JwkToPem(const std::string& n_b64url, const std::string& e_b64url) {
  std::string n_bytes = Base64UrlDecode(n_b64url);
  std::string e_bytes = Base64UrlDecode(e_b64url);
  if (n_bytes.empty() || e_bytes.empty()) {
    throw std::runtime_error("JWK n/e base64url decode produced empty");
  }

  BignumPtr n{BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes.data()),
                        static_cast<int>(n_bytes.size()), nullptr)};
  BignumPtr e{BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes.data()),
                        static_cast<int>(e_bytes.size()), nullptr)};
  if (!n || !e) throw std::runtime_error("BN_bin2bn failed for JWK n/e");

  OSSL_PARAM_BLD_PTR bld{OSSL_PARAM_BLD_new()};
  if (!bld) throw std::runtime_error("OSSL_PARAM_BLD_new failed");
  if (!OSSL_PARAM_BLD_push_BN(bld.get(), "n", n.get())) {
    throw std::runtime_error("OSSL_PARAM_BLD_push_BN(n) failed");
  }
  if (!OSSL_PARAM_BLD_push_BN(bld.get(), "e", e.get())) {
    throw std::runtime_error("OSSL_PARAM_BLD_push_BN(e) failed");
  }
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld.get());
  if (!params) throw std::runtime_error("OSSL_PARAM_BLD_to_param failed");
  std::unique_ptr<OSSL_PARAM, void (*)(OSSL_PARAM*)> params_ptr{
      params, OSSL_PARAM_free};

  EvpPkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_name(/*libctx=*/nullptr, "RSA",
                                               /*propq=*/nullptr)};
  if (!ctx) {
    throw std::runtime_error("EVP_PKEY_CTX_new_from_name(\"RSA\") failed");
  }
  if (EVP_PKEY_fromdata_init(ctx.get()) <= 0) {
    throw std::runtime_error("EVP_PKEY_fromdata_init failed");
  }

  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
    throw std::runtime_error("EVP_PKEY_fromdata failed");
  }
  EvpPkeyPtr pkey{raw};

  BioPtr bio{BIO_new(BIO_s_mem())};
  if (!bio) throw std::runtime_error("BIO_new(BIO_s_mem) failed");
  if (PEM_write_bio_PUBKEY(bio.get(), pkey.get()) != 1) {
    throw std::runtime_error("PEM_write_bio_PUBKEY failed");
  }

  char* buf = nullptr;
  long n_out = BIO_get_mem_data(bio.get(), &buf);
  if (n_out <= 0 || !buf) throw std::runtime_error("BIO_get_mem_data failed");
  return std::string(buf, static_cast<size_t>(n_out));
}

}  // namespace

// --- JwksCache -------------------------------------------------------------

JwksCache::JwksCache(std::string url, std::chrono::seconds ttl)
    : url_(std::move(url)), ttl_(ttl) {}

void JwksCache::refresh() {
  auto body = FetchUrl(url_);
  auto j = nlohmann::json::parse(body);

  std::unordered_map<std::string, std::string> next;
  if (j.is_object() && j.contains("keys") && j["keys"].is_array()) {
    for (const auto& k : j["keys"]) {
      if (!k.is_object()) continue;
      if (k.value("kty", std::string{}) != "RSA") continue;
      const std::string kid = k.value("kid", std::string{});
      if (kid.empty()) continue;
      if (!k.contains("n") || !k.contains("e")) continue;
      if (!k["n"].is_string() || !k["e"].is_string()) continue;
      next[kid] = JwkToPem(k["n"].get<std::string>(),
                           k["e"].get<std::string>());
    }
  }

  keys_ = std::move(next);
  fetched_at_ = std::chrono::steady_clock::now();
}

std::optional<std::string> JwksCache::GetKey(const std::string& kid) {
  std::lock_guard<std::mutex> lk(mu_);

  const auto now = std::chrono::steady_clock::now();
  const bool expired =
      fetched_at_.time_since_epoch().count() == 0  // never fetched
      || (now - fetched_at_) > ttl_;

  auto it = keys_.find(kid);
  if (it != keys_.end() && !expired) return it->second;

  // Cache miss or expired entry: refresh. Failures are swallowed (logged
  // silently) so a temporarily-unreachable IdP doesn't crash the gRPC
  // server — the JwtValidator returns nullopt either way and the request
  // is rejected with UNAUTHENTICATED.
  try {
    refresh();
  } catch (const std::exception&) {
    return std::nullopt;
  }

  it = keys_.find(kid);
  if (it != keys_.end()) return it->second;
  return std::nullopt;
}

}  // namespace evgrpc