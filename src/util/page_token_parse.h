#pragma once
#include <string>

#include <grpcpp/support/status.h>

namespace evgrpc {

// Parses a pagination `page_token` string into a non-negative integer
// offset, used by the List RPCs (ListVehicles, ListChargings,
// ListConsumptions).
//
// The page_token contract is opaque to the client, but its on-the-wire
// representation is `std::to_string(offset + page_size)` — i.e. a
// decimal integer encoding the *next* offset. Anything else is
// malformed and MUST be rejected with INVALID_ARGUMENT, not INTERNAL.
//
// Returns:
//   - grpc::Status::OK and writes the parsed offset to *out_offset
//     when the input is empty OR a non-negative integer.
//   - grpc::Status(INVALID_ARGUMENT, "page_token must be a non-negative
//     integer; got '<input>'") otherwise. Negative offsets are clamped
//     to 0 (the empty-token path is the canonical "first page").
//
// Why not `std::stoi` directly? It throws `std::invalid_argument` (on
// non-numeric input) or `std::out_of_range` (on overflow), both of which
// hit the generic catch in service RPCs and surface as INTERNAL —
// "the server crashed" — which is misleading: the user just sent a
// bad token.
grpc::Status ParsePageToken(const std::string& token, int* out_offset);

}  // namespace evgrpc