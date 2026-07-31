#pragma once
#include <string>

namespace evgrpc {

// Generates a fresh RFC 4122 version-4 UUID string in lowercase
// 8-4-4-4-12 hex format (36 chars total, including dashes).
// Wraps libuuid's uuid_generate + uuid_unparse_lower.
std::string NewUuid();

}  // namespace evgrpc