#include "fixtures/test_server.h"

#include <grpc/grpc_security_constants.h>

#include <httplib.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "auth/jwt_validator.h"
#include "db/pool.h"
#include "evgrpc/charging.grpc.pb.h"
#include "evgrpc/consumption.grpc.pb.h"
#include "evgrpc/display.grpc.pb.h"
#include "evgrpc/source_category.grpc.pb.h"
#include "evgrpc/vehicle.grpc.pb.h"
#include "evgrpc/weather.grpc.pb.h"
#include "fixtures/jwt_test_keys.h"
#include "fixtures/pg_container.h"
#include "services/charging_service.h"
#include "services/consumption_service.h"
#include "services/display_service.h"
#include "services/source_category_service.h"
#include "services/vehicle_service.h"
#include "services/weather_service.h"

namespace evgrpc::test {
namespace {

// RAII aliases for OpenSSL types — declared at the top of the anonymous
// namespace so they're visible to all helpers below (notably
// PemToJwksJson, which uses BIOPtr / EvpPKeyPtr / BIGNUMPtr).
using BIOPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using EvpPKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BIGNUMPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

// Base64url (no padding) encoder for binary data → JWKS `n` / `e`.
// RFC 7518 §2: base64url is base64 with `+→-`, `/→_`, `=→""`.
std::string Base64UrlNoPad(const unsigned char* data, size_t len) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t triplet =
        (static_cast<uint32_t>(data[i]) << 16) |
        (i + 1 < len ? (static_cast<uint32_t>(data[i + 1]) << 8) : 0) |
        (i + 2 < len ? static_cast<uint32_t>(data[i + 2]) : 0);
    out.push_back(kAlphabet[(triplet >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triplet >> 12) & 0x3f]);
    if (i + 1 < len) out.push_back(kAlphabet[(triplet >> 6) & 0x3f]);
    if (i + 2 < len) out.push_back(kAlphabet[triplet & 0x3f]);
  }
  return out;
}

// Parse an RSA public-key PEM (SubjectPublicKeyInfo,
// "-----BEGIN PUBLIC KEY-----") and emit a one-key JWKS JSON document.
// Returns the JWKS as a string. Throws std::runtime_error on parse error.
std::string PemToJwksJson(const std::string& pem_public,
                          const std::string& kid) {
  // Read PEM into EVP_PKEY.
  BIOPtr bio{BIO_new_mem_buf(pem_public.data(),
                             static_cast<int>(pem_public.size())), BIO_free};
  if (!bio) throw std::runtime_error("PemToJwksJson: BIO_new_mem_buf failed");

  EvpPKeyPtr pkey{PEM_read_bio_PUBKEY_ex(
      bio.get(), /*x=*/nullptr, /*cb=*/nullptr, /*u=*/nullptr,
      /*libctx=*/nullptr, /*propq=*/nullptr), EVP_PKEY_free};
  if (!pkey) {
    throw std::runtime_error(
        "PemToJwksJson: PEM_read_bio_PUBKEY_ex failed (malformed PEM?)");
  }

  // Extract the RSA key (we only support RSA for the test IdP; matches
  // jwt-cpp's RS256 algorithm).
  //
  // EVP_PKEY_get_bn_param writes a BIGNUM* into a BIGNUM** slot. The
  // simplest RAII is to allocate BIGNUM* ourselves, pass &bignum, then
  // wrap the result in a unique_ptr with the matching deleter type.
  BIGNUM* n_raw = BN_new();
  BIGNUM* e_raw = BN_new();
  BIGNUMPtr n_bn{n_raw, BN_free};
  BIGNUMPtr e_bn{e_raw, BN_free};
  if (!n_raw || !e_raw) throw std::runtime_error("PemToJwksJson: BN_new failed");
  if (EVP_PKEY_get_bn_param(pkey.get(), "n", &n_raw) != 1 ||
      EVP_PKEY_get_bn_param(pkey.get(), "e", &e_raw) != 1) {
    throw std::runtime_error(
        "PemToJwksJson: EVP_PKEY_get_bn_param(n|e) failed (non-RSA key?)");
  }

  // Serialize to big-endian unsigned bytes (no leading zero — OpenSSL
  // strips it automatically; we just take the BN's magnitude bytes).
  auto bn_to_bytes = [](const BIGNUM* b) {
    std::string out(BN_num_bytes(b), '\0');
    const int n = BN_bn2bin(b, reinterpret_cast<unsigned char*>(out.data()));
    out.resize(static_cast<size_t>(n));
    return out;
  };
  const std::string n_bytes = bn_to_bytes(n_raw);
  const std::string e_bytes = bn_to_bytes(e_raw);

  // Build the JWKS JSON manually — no need to pull in nlohmann_json for
  // ~150 bytes of static output. Match what jwt-cpp expects on the
  // validate side (alg=RS256, use=sig, kty=RSA, kid, n, e).
  std::string json;
  json.reserve(256);
  json += "{\"keys\":[{\"kty\":\"RSA\",\"use\":\"sig\",\"alg\":\"RS256\",";
  json += "\"kid\":\"";
  json += kid;
  json += "\",\"n\":\"";
  json += Base64UrlNoPad(reinterpret_cast<const unsigned char*>(n_bytes.data()),
                         n_bytes.size());
  json += "\",\"e\":\"";
  json += Base64UrlNoPad(reinterpret_cast<const unsigned char*>(e_bytes.data()),
                         e_bytes.size());
  json += "\"}]}";
  return json;
}

// Bearer-token credentials plugin: signs a fresh token on every GetMetadata
// call. Tokens are short-lived (exp = now + 1h); signing takes ~ms with
// the cached keypair, so we don't bother caching the token string.
class BearerTokenPlugin final : public grpc::MetadataCredentialsPlugin {
 public:
  explicit BearerTokenPlugin(const TestServer* server) : server_(server) {}

  // Default IsBlocking() returns true: our token signing is synchronous
  // and ~ms-fast, so the extra thread-hop is wasted overhead. Keep it
  // blocking.

  grpc::Status GetMetadata(
      grpc::string_ref /*service_url*/, grpc::string_ref /*method_name*/,
      const grpc::AuthContext& /*channel_auth_context*/,
      std::multimap<grpc::string, grpc::string>* metadata) override {
    metadata->insert({"authorization", "Bearer " + server_->SignToken()});
    return grpc::Status::OK;
  }

 private:
  const TestServer* server_;
};

}  // namespace

class TestServer::Impl {
 public:
  Impl(std::shared_ptr<PgContainer> pg) {
    // 1. RSA keypair — generated once, used for all token signing +
    //    JWKS serving during this fixture's lifetime.
    keypair_ = GenerateRsaKeyPair("test-kid");
    issuer_ = "https://test-idp";
    audience_ = "evgrpc";

    // 2. cpp-httplib HTTP server on a random localhost port.
    //    httplib::Server::bind_to_any_port picks port 0 and resolves
    //    the kernel-assigned port; we capture the returned int.
    jwks_http_ = std::make_unique<httplib::Server>();
    const std::string jwks_json =
        PemToJwksJson(keypair_.pem_public, keypair_.kid);
    jwks_http_->Get("/.well-known/jwks.json",
                    [jwks_json](const httplib::Request&,
                                httplib::Response& res) {
                      res.set_content(jwks_json, "application/json");
                    });
    jwks_http_->Get("/jwks.json",
                    [jwks_json](const httplib::Request&,
                                httplib::Response& res) {
                      // Convenience alias for tests that hardcode
                      // "/jwks.json" rather than the OIDC well-known
                      // path.
                      res.set_content(jwks_json, "application/json");
                    });
    const int http_port = jwks_http_->bind_to_any_port("127.0.0.1");
    if (http_port <= 0) {
      throw std::runtime_error("TestServer: failed to bind JWKS HTTP port");
    }
    // httplib::Server::listen_after_bind() runs the accept loop on the
    // calling thread. Spawn a daemon thread and detach; the server
    // lifetime is bound to jwks_http_ (RAII + ~TestServer::Impl calls
    // jwks_http_->stop()).
    std::thread([s = jwks_http_.get()]() { s->listen_after_bind(); }).detach();
    jwks_url_ = "http://127.0.0.1:" + std::to_string(http_port) +
                "/.well-known/jwks.json";

    // 3. JwtValidator. resolve_key reads from a static map keyed by
    //    `kid`; the map is populated once and never changes for the
    //    lifetime of the fixture.
    static const std::unordered_map<std::string, std::string>* kKeyMap =
        new std::unordered_map<std::string, std::string>{
            {keypair_.kid, keypair_.pem_public}};
    validator_.issuer = issuer_;
    validator_.audience = audience_;
    validator_.resolve_key = [](const std::string& kid)
        -> std::optional<std::string> {
      auto it = kKeyMap->find(kid);
      if (it == kKeyMap->end()) return std::nullopt;
      return it->second;
    };

    // 4. PgPool wired to the test container's libpq conninfo.
    pool_ = std::make_unique<PgPool>(pg->Conninfo(), /*size=*/2);

    // 5. gRPC server with all 6 services.
    grpc::ServerBuilder builder;
    int bound_port = 0;
    builder.SetMaxReceiveMessageSize(16 * 1024 * 1024);
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &bound_port);
    auto vehicle =
        std::make_unique<VehicleServiceImpl>(pool_.get(), &validator_);
    auto weather =
        std::make_unique<WeatherServiceImpl>(pool_.get(), &validator_);
    auto source_category =
        std::make_unique<SourceCategoryServiceImpl>(pool_.get(), &validator_);
    auto consumption =
        std::make_unique<ConsumptionServiceImpl>(pool_.get(), &validator_);
    auto charging =
        std::make_unique<ChargingServiceImpl>(pool_.get(), &validator_);
    auto display =
        std::make_unique<DisplayServiceImpl>(pool_.get(), &validator_);
    builder.RegisterService(vehicle.get());
    builder.RegisterService(weather.get());
    builder.RegisterService(source_category.get());
    builder.RegisterService(consumption.get());
    builder.RegisterService(charging.get());
    builder.RegisterService(display.get());

    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
      throw std::runtime_error("TestServer: BuildAndStart returned null");
    }
    grpc_port_ = static_cast<uint16_t>(bound_port);
    // Hold ownership of the services; the gRPC server only borrows
    // them via RegisterService (raw pointers). Push into the vector
    // explicitly because std::initializer_list assignment doesn't
    // compile with non-copyable unique_ptr.
    services_.push_back(std::move(vehicle));
    services_.push_back(std::move(weather));
    services_.push_back(std::move(source_category));
    services_.push_back(std::move(consumption));
    services_.push_back(std::move(charging));
    services_.push_back(std::move(display));
  }

  ~Impl() {
    if (grpc_server_) grpc_server_->Shutdown();
    if (jwks_http_) jwks_http_->stop();
  }

  RsaKeyPair keypair_;
  std::string issuer_;
  std::string audience_;
  std::string jwks_url_;
  uint16_t grpc_port_{};
  std::unique_ptr<PgPool> pool_;
  JwtValidator validator_;
  std::unique_ptr<httplib::Server> jwks_http_;
  std::unique_ptr<grpc::Server> grpc_server_;
  std::vector<std::unique_ptr<grpc::Service>> services_;
};

TestServer::TestServer(std::shared_ptr<PgContainer> pg)
    : impl_(std::make_unique<Impl>(std::move(pg))) {}
TestServer::~TestServer() = default;

const RsaKeyPair& TestServer::KeyPair() const noexcept {
  return impl_->keypair_;
}

const std::string& TestServer::Issuer() const noexcept {
  return impl_->issuer_;
}

const std::string& TestServer::Audience() const noexcept {
  return impl_->audience_;
}

const std::string& TestServer::OauthIssuerUrl() const noexcept {
  return impl_->jwks_url_;
}

const std::string& TestServer::JwksUrl() const noexcept {
  return impl_->jwks_url_;
}

uint16_t TestServer::GrpcPort() const noexcept {
  return impl_->grpc_port_;
}

std::string TestServer::SignToken() const {
  return SignJwt(KeyPair(), Issuer(), Audience(), /*exp_offset=*/3600);
}

std::string TestServer::SignTokenWith(const std::string& issuer,
                                      const std::string& audience,
                                      int64_t exp_offset_seconds) const {
  return SignJwt(KeyPair(), issuer, audience, exp_offset_seconds);
}

std::shared_ptr<grpc::Channel> TestServer::Channel() const {
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(16 * 1024 * 1024);
  // The server uses InsecureServerCredentials (no TLS); see
  // BearerTokenCredentials() for why we can't combine call creds at
  // this layer.
  return grpc::CreateCustomChannel(
      "127.0.0.1:" + std::to_string(impl_->grpc_port_),
      grpc::InsecureChannelCredentials(), args);
}

std::shared_ptr<grpc::CallCredentials>
TestServer::BearerTokenCredentials() const {
  // Per-call credentials sign a fresh RS256 JWT on every GetMetadata()
  // invocation. Callers must attach these to each grpc::ClientContext
  // via set_credentials() before issuing an RPC.
  return grpc::experimental::MetadataCredentialsFromPlugin(
      std::make_unique<BearerTokenPlugin>(this), GRPC_SECURITY_NONE);
}

}  // namespace evgrpc::test