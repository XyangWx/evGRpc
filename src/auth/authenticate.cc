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
}  // namespace

grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator,
    Claims* out_claims,
    std::string* out_reason) {
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