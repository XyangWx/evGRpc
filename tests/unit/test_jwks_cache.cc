#include <gtest/gtest.h>

#include "auth/jwks_cache.h"
#include "fixtures/jwt_test_keys.h"

#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <unistd.h>

using evgrpc::JwksCache;
using evgrpc::test::GenerateRsaKeyPair;
using evgrpc::test::RsaKeyPair;

namespace {

// --- OpenSSL RAII ----------------------------------------------------------
struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept {
    if (p) EVP_PKEY_free(p);
  }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct BignumDeleter {
  void operator()(BIGNUM* b) const noexcept {
    if (b) BN_free(b);
  }
};
using BignumPtr = std::unique_ptr<BIGNUM, BignumDeleter>;

struct BioDeleter {
  void operator()(BIO* b) const noexcept {
    if (b) BIO_free(b);
  }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// --- base64url helpers -----------------------------------------------------
//
// JWK uses RFC 7515 base64url (no padding). Standard base64 uses '+', '/',
// and '=' padding, so encode to standard then rewrite the alphabet.

std::string Base64UrlEncode(const unsigned char* data, size_t len) {
  BioPtr bio{BIO_new(BIO_s_mem())};
  if (!bio) throw std::runtime_error("BIO_new(BIO_s_mem) failed");
  BioPtr b64{BIO_new(BIO_f_base64())};
  if (!b64) throw std::runtime_error("BIO_new(BIO_f_base64) failed");
  BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);
  BIO* chain = BIO_push(b64.release(), bio.release());
  BioPtr chain_ptr{chain};

  BIO_write(chain_ptr.get(), data, static_cast<int>(len));
  BIO_flush(chain_ptr.get());

  char* buf = nullptr;
  long n = BIO_get_mem_data(chain_ptr.get(), &buf);
  std::string out(buf, static_cast<size_t>(n));
  // Rewrite alphabet and strip padding.
  for (char& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

// --- PEM → JWK component extraction ----------------------------------------
//
// The fixture generates a 2048-bit RSA keypair and gives us the PEM-encoded
// public key (SubjectPublicKeyInfo). The test needs to publish that key
// inside a JWKS JSON, which requires the modulus (`n`) and public exponent
// (`e`) as base64url-encoded unsigned big-endian integers, exactly as
// OpenSSL stores them internally.

std::string Base64UrlEncodeBignum(const BIGNUM* bn) {
  int len = BN_num_bytes(bn);
  if (len <= 0) throw std::runtime_error("BN_num_bytes returned non-positive");
  std::string bytes(static_cast<size_t>(len), '\0');
  BN_bn2bin(bn, reinterpret_cast<unsigned char*>(bytes.data()));
  return Base64UrlEncode(reinterpret_cast<const unsigned char*>(bytes.data()),
                         bytes.size());
}

struct JwkRsaComponents {
  std::string n;  // base64url
  std::string e;  // base64url
};

JwkRsaComponents ExtractRsaComponents(const std::string& pem_public) {
  BioPtr bio{BIO_new_mem_buf(pem_public.data(),
                             static_cast<int>(pem_public.size()))};
  if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");

  EvpPkeyPtr pkey{
      PEM_read_bio_PUBKEY(bio.get(), /*x=*/nullptr, /*cb=*/nullptr, /*u=*/nullptr)};
  if (!pkey) throw std::runtime_error("PEM_read_bio_PUBKEY failed");

  // OpenSSL 3.0+ accessor: returns 1 on success and writes a freshly
  // allocated BIGNUM* into *bn. The caller takes ownership (BN_free).
  BIGNUM* n_raw = nullptr;
  BIGNUM* e_raw = nullptr;
  if (EVP_PKEY_get_bn_param(pkey.get(), "n", &n_raw) != 1) {
    throw std::runtime_error("EVP_PKEY_get_bn_param(n) failed");
  }
  if (EVP_PKEY_get_bn_param(pkey.get(), "e", &e_raw) != 1) {
    BN_free(n_raw);
    throw std::runtime_error("EVP_PKEY_get_bn_param(e) failed");
  }
  BignumPtr n{n_raw};
  BignumPtr e{e_raw};

  return JwkRsaComponents{Base64UrlEncodeBignum(n.get()),
                          Base64UrlEncodeBignum(e.get())};
}

// --- JWKS JSON builder -----------------------------------------------------
//
// Builds {"keys":[{"kty":"RSA","kid":...,"n":...,"e":...}, ...]}. Only RSA
// is supported (RS256 is the only algorithm JWT validator accepts).

std::string BuildJwks(const std::vector<
    std::tuple<std::string, std::string, std::string>>& keys) {
  nlohmann::json j;
  j["keys"] = nlohmann::json::array();
  for (const auto& [kid, n, e] : keys) {
    j["keys"].push_back({
        {"kty", "RSA"},
        {"use", "sig"},
        {"alg", "RS256"},
        {"kid", kid},
        {"n", n},
        {"e", e},
    });
  }
  return j.dump();
}

// --- Temp file helpers -----------------------------------------------------

struct TempJwksFile {
  std::filesystem::path path;
  TempJwksFile(const std::string& contents) {
    // mkstemp gives an atomic, exclusive, world-readable file we can
    // safely hand a file:// URL to.
    std::string tmpl = (std::filesystem::temp_directory_path() /
                        "evgrpc-jwks-XXXXXX")
                           .string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    close(fd);
    path = buf.data();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
  }
  ~TempJwksFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempJwksFile(const TempJwksFile&) = delete;
  TempJwksFile& operator=(const TempJwksFile&) = delete;
};

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The happy path: a JWKS containing one key is fetched, the kid lookup
// returns the PEM. Uses file:// (curl supports it natively) so the test
// doesn't need a free port or an HTTP server.
TEST(JwksCacheTest, CachesKeyAfterFirstFetch) {
  const auto key = GenerateRsaKeyPair("k1");
  const auto comps = ExtractRsaComponents(key.pem_public);

  const std::string body = BuildJwks({{"k1", comps.n, comps.e}});
  TempJwksFile file{body};

  JwksCache cache("file://" + file.path.string(), std::chrono::seconds(3600));
  auto got = cache.GetKey("k1");

  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, key.pem_public);
}

// After the TTL expires the next GetKey call refetches. We verify by
// publishing different keys in the served JWKS before/after the wait.
TEST(JwksCacheTest, RefreshesAfterTtl) {
  const auto key1 = GenerateRsaKeyPair("k1");
  const auto key2 = GenerateRsaKeyPair("k2");
  const auto c1 = ExtractRsaComponents(key1.pem_public);
  const auto c2 = ExtractRsaComponents(key2.pem_public);

  TempJwksFile file{BuildJwks({{"k1", c1.n, c1.e}})};

  // ttl = 1s — long enough to amortize across scheduler jitter but
  // short enough to keep the test fast.
  JwksCache cache("file://" + file.path.string(), std::chrono::seconds(1));

  auto got1 = cache.GetKey("k1");
  ASSERT_TRUE(got1.has_value());
  EXPECT_EQ(*got1, key1.pem_public);

  // Replace served content with a different key.
  {
    std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
    auto body = BuildJwks({{"k2", c2.n, c2.e}});
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }

  // Wait past TTL. The cache should refetch on next GetKey.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  auto got2 = cache.GetKey("k2");
  ASSERT_TRUE(got2.has_value());
  EXPECT_EQ(*got2, key2.pem_public);

  // The previously-cached "k1" is gone (the served JWKS no longer
  // contains it).
  auto missing = cache.GetKey("k1");
  EXPECT_FALSE(missing.has_value());
}

// Unknown kid on a populated JWKS returns nullopt without retrying
// (the cache hit case is exercised).
TEST(JwksCacheTest, UnknownKidReturnsNullopt) {
  const auto key = GenerateRsaKeyPair("only-this-kid");
  const auto comps = ExtractRsaComponents(key.pem_public);
  TempJwksFile file{BuildJwks({{"only-this-kid", comps.n, comps.e}})};

  JwksCache cache("file://" + file.path.string(), std::chrono::seconds(3600));

  EXPECT_FALSE(cache.GetKey("not-in-jwks").has_value());
  // The known kid still works.
  auto got = cache.GetKey("only-this-kid");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, key.pem_public);
}

// Malformed JSON yields nullopt without crashing the cache.
TEST(JwksCacheTest, MalformedJwksYieldsNullopt) {
  TempJwksFile file{"this is not json {"};
  JwksCache cache("file://" + file.path.string(), std::chrono::seconds(3600));
  EXPECT_FALSE(cache.GetKey("anything").has_value());
}

// Missing file (file:// pointing at a non-existent path) yields nullopt
// without crashing.
TEST(JwksCacheTest, MissingFileYieldsNullopt) {
  // mkstemp gives us a unique, definitely-doesn't-exist-after-remove path
  // without hand-rolling collision-resistant suffixes. Store the path in
  // a local first; using a path's `.string().begin()` inline returns
  // iterators into a temporary that dies before the vector constructor
  // runs (caught by libstdc++'s debug-mode vector size assert).
  std::string tmpl = (std::filesystem::temp_directory_path() /
                      "evgrpc-jwks-missing-XXXXXX")
                         .string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd >= 0) close(fd);
  std::filesystem::remove(buf.data());

  JwksCache cache("file://" + std::string(buf.data()),
                  std::chrono::seconds(3600));
  EXPECT_FALSE(cache.GetKey("k1").has_value());
}