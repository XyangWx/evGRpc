#pragma once
#include <optional>
#include <string>
#include <google/protobuf/timestamp.pb.h>

namespace evgrpc {

// Returns a std::optional<string> for a proto Timestamp filter binding.
//
// `has_value` should be the result of the parent message's
// `has_<field_name>()` (e.g., `req->has_start_time()` for the start_time
// field of a GetXxxRequest). This is needed because the proto3 default
// Timestamp value (seconds=0, nanos=0) is indistinguishable from an
// EXPLICITLY-set 1970-01-01T00:00:00Z — both have seconds() == 0 and
// nanos() == 0, but they mean different things to the SQL filter:
//   * has_value == false  → user didn't set the field → no filter
//   * has_value == true   → user set the field (e.g., 1970-01-01) → use the value
//
// libpqxx binds std::optional<std::string> as either the value or NULL,
// so the SQL `($N::TIMESTAMP IS NULL OR ...)` predicates can be
// uniformly "no filter" or "this exact value".
std::optional<std::string> MaybeTimestamp(
    bool has_value, const google::protobuf::Timestamp& ts);

}  // namespace evgrpc
