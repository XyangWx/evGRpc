#include <gtest/gtest.h>
#include "util/maybe_timestamp.h"
#include <google/protobuf/timestamp.pb.h>
#include <ctime>
#include <string>

namespace {

using evgrpc::MaybeTimestamp;

// Helper: build a Timestamp with explicit seconds (so has_xxx is true).
google::protobuf::Timestamp MakeTs(int64_t seconds, int32_t nanos) {
  google::protobuf::Timestamp ts;
  ts.set_seconds(seconds);
  ts.set_nanos(nanos);
  return ts;
}

TEST(MaybeTimestamp, UnsetWhenHasValueIsFalse) {
  // has_value=false means the parent message didn't set the field.
  // Even if the Timestamp is "set" (e.g., to 2024-01-01), if the parent
  // didn't set it, we treat as no filter.
  auto ts = MakeTs(1704067200, 0);  // 2024-01-01T00:00:00Z
  auto result = MaybeTimestamp(/*has_value=*/false, ts);
  EXPECT_FALSE(result.has_value());
}

TEST(MaybeTimestamp, HasValueAndExplicitlySetReturnsString) {
  // has_value=true + non-default Timestamp → returns the ISO 8601 string.
  auto ts = MakeTs(1704067200, 0);
  auto result = MaybeTimestamp(/*has_value=*/true, ts);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(*result, "2024-01-01T00:00:00Z");
}

TEST(MaybeTimestamp, Epoch1970IsNotTreatedAsUnset) {
  // The proto3 default value (epoch) is indistinguishable from an
  // EXPLICITLY-set 1970-01-01T00:00:00Z by just checking seconds/nanos.
  // The fix: pass the parent's has_xxx() result as a separate argument.
  //   * has_value=false → no filter (default was used)
  //   * has_value=true  → use the value (user explicitly set 1970-01-01)
  auto ts = MakeTs(0, 0);  // epoch, same as default
  EXPECT_FALSE(MaybeTimestamp(/*has_value=*/false, ts).has_value());
  EXPECT_TRUE(MaybeTimestamp(/*has_value=*/true, ts).has_value());
  EXPECT_EQ(*MaybeTimestamp(true, ts), "1970-01-01T00:00:00Z");
}

TEST(MaybeTimestamp, EpochPlusOneSecondWorks) {
  // 1 second past epoch (a real timestamp a user might set).
  auto ts = MakeTs(1, 0);
  auto result = MaybeTimestamp(/*has_value=*/true, ts);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(*result, "1970-01-01T00:00:01Z");
}

TEST(MaybeTimestamp, CurrentTimeWorks) {
  // Current time: should serialize correctly.
  auto ts = google::protobuf::Timestamp();
  ts.set_seconds(std::time(nullptr));
  auto result = MaybeTimestamp(true, ts);
  EXPECT_TRUE(result.has_value());
  // Format: "YYYY-MM-DDTHH:MM:SSZ"
  EXPECT_EQ(result->back(), 'Z');
}

}  // namespace
