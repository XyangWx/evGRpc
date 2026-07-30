#include "auth/authenticate.h"

#include <string>
#include <string_view>

namespace evgrpc {

namespace {
constexpr char kAuthHeader[] = "authorization";
constexpr char kBearerPrefix[] = "Bearer ";
}  // namespace

grpc::Status Authenticate(
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata,
    const JwtValidator& validator) {
  auto it = client_metadata.find(grpc::string_ref(kAuthHeader));
  if (it == client_metadata.end()) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "missing authorization header");
  }

  // zero-copy grpc::string_ref → std::string_view for safe prefix/suffix work
  const std::string_view val(it->second.data(), it->second.size());
  constexpr std::string_view prefix(kBearerPrefix);

  if (val.size() <= prefix.size() ||
      val.substr(0, prefix.size()) != prefix) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "authorization must be 'Bearer <token>'");
  }

  const std::string token(val.substr(prefix.size()));
  auto claims = validator.Validate(token);
  if (!claims.has_value()) {
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                        "invalid bearer token");
  }
  return grpc::Status::OK;
}

}  // namespace evgrpc
