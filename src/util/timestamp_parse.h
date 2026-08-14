#pragma once

#include <string>

#include <google/protobuf/timestamp.pb.h>

namespace evgrpc {

// Parses a PostgreSQL TIMESTAMP / TIMESTAMPTZ `::text` representation
// into a protobuf `Timestamp` (UTC epoch seconds).
//
// Accepted forms (PG default DateStyle output):
//   "2023-11-14 22:13:20"           no fractional seconds, no offset
//   "2023-11-14 22:13:20.123456"    fractional seconds (dropped)
//   "2023-11-14 22:13:20+00"        UTC offset, hours only
//   "2023-11-14 22:13:20+05:30"     UTC offset, hours:minutes
//   "2023-11-14 22:13:20-08"        negative offset
//   "2023-11-14 22:13:20Z"          'Z' = UTC
//
// TIMESTAMPTZ `::text` renders the wall-clock time in the *session*
// time zone together with its UTC offset, so the offset MUST be applied
// to recover the correct UTC instant. A bare TIMESTAMP column has no
// offset and is interpreted as UTC (its stored wall-clock is treated
// as the instant; see the TIMESTAMPTZ migration note for the caveat).
//
// Fractional seconds are truncated (second precision) to match the
// existing round-trip contract.
//
// Returns true on success (writes *out), false on empty or malformed
// input.
bool ParseTimestamp(const std::string& s, google::protobuf::Timestamp* out);

}  // namespace evgrpc
