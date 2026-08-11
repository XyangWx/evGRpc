#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "fixtures/jwt_test_keys.h"

namespace evgrpc::test {

class PgContainer;

// In-process gRPC server with all 6 services + a test-only JWT IdP.
//
// Owns:
//   * one 2048-bit RSA keypair (re-used across the fixture's lifetime);
//   * an embedded HTTP server (cpp-httplib) bound to localhost:<random>
//     serving `GET /jwks.json` returning the JWKS for the keypair;
//   * an in-process grpc::Server bound to localhost:<random> with all 6
//     services registered and a JwtValidator whose `resolve_key` reads
//     from a static map keyed by `kid`;
//   * a PgPool wired to the supplied PgContainer.
//
// Tests obtain RPC access via Channel() — the returned Channel carries
// `Authorization: Bearer <valid-token>` on every call via a custom
// MetadataCredentialsPlugin. Tokens are minted on-demand (not cached),
// but signing is cheap (~ms) and the keypair is reused.
//
// Thread-safety: TestServer is intended to be used from a single test
// thread at a time. Multi-threaded usage is not supported in v1.
class TestServer {
 public:
  // Per-instance configuration for the in-process gRPC fixture. The
  // `no_auth` flag flips JwtValidator into test-mode bypass so RPCs
  // issued without bearer-token credentials succeed end-to-end; tests
  // that don't care about auth (e.g. service-shape coverage) use this
  // path so they don't have to mint an RS256 JWT per call. When false,
  // the existing JWKS HTTP server bringup runs unchanged.
  //
  // Nested inside TestServer (not a free TestServerOptions) because it
  // is exclusively a test-fixture knob and should not leak into the
  // production namespace.
  struct Options {
    bool no_auth = false;
    std::shared_ptr<PgContainer> pg;
  };
  explicit TestServer(Options opts);
  explicit TestServer(std::shared_ptr<PgContainer> pg);
  ~TestServer();
  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;
  TestServer(TestServer&&) = delete;
  TestServer& operator=(TestServer&&) = delete;

  // Returns an insecure channel pointed at the in-process server.
  // Bearer-token credentials must be attached to each ClientContext
  // separately via BearerTokenCredentials() — see below.
  std::shared_ptr<grpc::Channel> Channel() const;

  // Per-call bearer-token credentials. Tests must attach this to every
  // grpc::ClientContext before issuing an RPC:
  //   grpc::ClientContext ctx;
  //   ctx.set_credentials(ts.BearerTokenCredentials());
  //   stub->SomeRpc(&ctx, ...);
  //
  // Why this dance instead of `CompositeChannelCredentials(insecure,
  // call_creds)`: CompositeChannelCredentials internally calls
  // AsSecureCredentials() on both sides and returns nullptr if either
  // isn't "secure". InsecureChannelCredentials() returns nullptr from
  // that method, so the composite is dropped — and so are the call
  // credentials. The fix is to attach the call credentials to the
  // ClientContext, which is the documented pattern for insecure+auth.
  std::shared_ptr<grpc::CallCredentials> BearerTokenCredentials() const;

  // Sign a token directly (no channel needed). Useful for tests that
  // want to assert on the validator path with custom iss/aud/exp.
  std::string SignToken() const;
  std::string SignTokenWith(const std::string& issuer,
                            const std::string& audience,
                            int64_t exp_offset_seconds) const;

  const RsaKeyPair& KeyPair() const noexcept;
  const std::string& Issuer() const noexcept;
  const std::string& Audience() const noexcept;
  // JWKS URL (the test IdP's well-known endpoint). Same as JwksUrl().
  const std::string& OauthIssuerUrl() const noexcept;
  const std::string& JwksUrl() const noexcept;
  uint16_t GrpcPort() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace evgrpc::test