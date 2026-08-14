#include <gtest/gtest.h>

#include <string>

#include "util/timestamp_parse.h"

namespace evgrpc {

namespace {

// Parse and return epoch seconds, or -1 on parse failure (ParseTimestamp
// does not write `out` on failure, so a bare seconds() read would be 0).
long ParseSeconds(const std::string& s) {
  google::protobuf::Timestamp ts;
  if (!ParseTimestamp(s, &ts)) return -1;
  return static_cast<long>(ts.seconds());
}

}  // namespace

// 1700000000 == 2023-11-14 22:13:20 UTC (the fixed instant used by the
// integration round-trip test).

TEST(ParseTimestampTest, NoOffset) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20"), 1700000000L);
}

TEST(ParseTimestampTest, FractionalSecondsDropped) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20.123456"), 1700000000L);
}

// Regression: fractional seconds must not swallow a non-zero offset.
// The original body.resize(dot) truncated the offset too, reintroducing
// the 8h shift for any sub-second-precision row under a non-UTC session.
TEST(ParseTimestampTest, FractionalSecondsWithPositiveOffset) {
  EXPECT_EQ(ParseSeconds("2023-11-15 06:13:20.123456+08"), 1700000000L);
}

TEST(ParseTimestampTest, FractionalSecondsWithPositiveOffsetMinutes) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20.500000+05:30"), 1699980200L);
}

TEST(ParseTimestampTest, FractionalSecondsWithNegativeOffset) {
  EXPECT_EQ(ParseSeconds("2023-11-14 14:13:20.999999-08"), 1700000000L);
}

TEST(ParseTimestampTest, FractionalSecondsWithZuluOffset) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20.123456Z"), 1700000000L);
}

TEST(ParseTimestampTest, UtcOffsetZero) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20.000000+00"), 1700000000L);
}

TEST(ParseTimestampTest, ZuluOffset) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20Z"), 1700000000L);
}

// The core regression: a positive offset must be applied. Under session
// TZ=Asia/Shanghai, TIMESTAMPTZ::text renders the same instant as
// "2023-11-15 06:13:20+08", not the UTC wall-clock.
TEST(ParseTimestampTest, PositiveOffsetHours) {
  EXPECT_EQ(ParseSeconds("2023-11-15 06:13:20+08"), 1700000000L);
}

TEST(ParseTimestampTest, PositiveOffsetHoursMinutes) {
  // 22:13:20+05:30 == 16:43:20 UTC.
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20+05:30"), 1699980200L);
}

TEST(ParseTimestampTest, NegativeOffsetHours) {
  // 14:13:20-08 == 22:13:20 UTC.
  EXPECT_EQ(ParseSeconds("2023-11-14 14:13:20-08"), 1700000000L);
}

TEST(ParseTimestampTest, EmptyFails) {
  EXPECT_EQ(ParseSeconds(""), -1L);
}

TEST(ParseTimestampTest, MalformedFails) {
  EXPECT_EQ(ParseSeconds("not a timestamp"), -1L);
}

TEST(ParseTimestampTest, TrailingGarbageFails) {
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20garbage"), -1L);
}

TEST(ParseTimestampTest, SingleDigitOffsetFails) {
  // "+8" is not a valid offset form (+HH requires two digits).
  EXPECT_EQ(ParseSeconds("2023-11-14 22:13:20+8"), -1L);
}

}  // namespace evgrpc
