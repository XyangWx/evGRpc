#include "util/page_token_parse.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

namespace evgrpc {

grpc::Status ParsePageToken(const std::string& token, int* out_offset) {
  // Empty token = first page (offset 0). Matches the existing pattern.
  if (token.empty()) {
    *out_offset = 0;
    return grpc::Status::OK;
  }

  // strtol gives us the same control over base/endptr/errno that
  // std::stoi does internally, but we can inspect the errno and
  // distinguish between invalid_argument (no digits) and out_of_range
  // (value > INT_MAX) without exception handling. Also lets us reject
  // empty strings explicitly (strtol with nptr="" returns 0 with
  // endptr == nptr; we'd rather treat that as malformed).
  if (token.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "page_token must be a non-negative integer; got ''");
  }
  errno = 0;
  char* end = nullptr;
  const long val = std::strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') {
    // No digits parsed, or trailing garbage after digits. Either way
    // the token is malformed.
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "page_token must be a non-negative integer; got '" +
                            token + "'");
  }
  if (errno == ERANGE || val < 0 || val > INT_MAX) {
    // Overflow or negative. Clamp negative to 0 (matches the existing
    // `if (offset < 0) offset = 0;` behavior in the 3 service callers),
    // but explicit overflow is rejected.
    if (val < 0) {
      *out_offset = 0;
      return grpc::Status::OK;
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "page_token value out of range; got '" + token + "'");
  }
  *out_offset = static_cast<int>(val);
  return grpc::Status::OK;
}

}  // namespace evgrpc