#include "util/maybe_timestamp.h"

#include <google/protobuf/util/time_util.h>

namespace evgrpc {

std::optional<std::string> MaybeTimestamp(
    bool has_value, const google::protobuf::Timestamp& ts) {
  if (!has_value) return std::nullopt;
  return google::protobuf::util::TimeUtil::ToString(ts);
}

}  // namespace evgrpc
