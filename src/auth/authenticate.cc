#include "auth/authenticate.h"

#include <string>
#include <string_view>

namespace evgrpc {

namespace {
constexpr char kAuthHeader[] = "authorization";
constexpr char kBearerPrefix[] = "Bearer ";

constexpr char kReasonOk[] = "ok";
constexpr char kReasonMissingHeader[] = "missing_header";
constexpr char kReasonNonBearer[] = "non_bearer";
constexpr char kReasonBadSignature[] = "bad_signature";
constexpr char kReasonBypass[] = "bypass";
}  // namespace

grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator,
    Claims* out_claims,
    std::string* out_reason) {
  // Test-mode bypass: when JwtValidator.bypass is set, skip the header
  // check entirely and synthesize claims from the validator's
  // configured iss/aud. The flag is only ever set in the TestServer
  // fixture (tests/fixtures/test_server.cc) — the §10.5 grep gate
  // enforces this — so production code paths are unaffected.
  if (validator.bypass) {
    if (out_claims) {
      *out_claims = Claims{
          /* subject  */ "test-subject",
          /* issuer   */ validator.issuer,
          /* audience */ validator.audience,
      };
    }
    if (out_reason) *out_reason = kReasonBypass;
    return grpc::Status::OK;
  }

  auto it = client_metadata.find(grpc::string_ref(kAuthHeader));
  if (it == client_metadata.end()) {
    if (out_reason) *out_reason = kReasonMissingHeader;
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "missing authorization header");
  }

  // zero-copy grpc::string_ref → std::string_view for safe prefix/suffix work
  const std::string_view val(it->second.data(), it->second.size());
  constexpr std::string_view prefix(kBearerPrefix);

  if (val.size() <= prefix.size() ||
      val.substr(0, prefix.size()) != prefix) {
    if (out_reason) *out_reason = kReasonNonBearer;
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "authorization must be 'Bearer <token>'");
  }

  const std::string token(val.substr(prefix.size()));
  auto claims = validator.Validate(token);
  if (!claims.has_value()) {
    if (out_reason) *out_reason = kReasonBadSignature;
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "invalid bearer token");
  }
  if (out_claims) {
    *out_claims = std::move(*claims);
  }
  if (out_reason) *out_reason = kReasonOk;
  return grpc::Status::OK;
}

}  // namespace evgrpc