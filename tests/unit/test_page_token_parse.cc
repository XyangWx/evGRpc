#include <gtest/gtest.h>
#include <climits>
#include "util/page_token_parse.h"

namespace {

TEST(PageTokenParse, EmptyTokenIsFirstPage) {
  // The canonical empty-token contract: first page, offset 0.
  int offset = -1;  // sentinel — should be reset to 0
  auto s = evgrpc::ParsePageToken("", &offset);
  EXPECT_TRUE(s.ok()) << s.error_message();
  EXPECT_EQ(offset, 0);
}

TEST(PageTokenParse, ZeroIsFirstPage) {
  int offset = -1;
  auto s = evgrpc::ParsePageToken("0", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 0);
}

TEST(PageTokenParse, ValidInteger) {
  int offset = -1;
  auto s = evgrpc::ParsePageToken("42", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 42);
}

TEST(PageTokenParse, ValidLargeInteger) {
  // Non-overflowing large value (INT_MAX / 2).
  int offset = -1;
  auto s = evgrpc::ParsePageToken("1000000", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 1000000);
}

TEST(PageTokenParse, NonNumericReturnsInvalidArgument) {
  // The Bug #4 fix: std::stoi used to throw invalid_argument →
  // caught generically → default INTERNAL. Now INVALID_ARGUMENT.
  int offset = -1;
  auto s = evgrpc::ParsePageToken("abc", &offset);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(s.error_message().find("non-negative integer"), std::string::npos)
      << s.error_message();
  EXPECT_NE(s.error_message().find("abc"), std::string::npos);
}

TEST(PageTokenParse, FloatPointReturnsInvalidArgument) {
  int offset = -1;
  auto s = evgrpc::ParsePageToken("1.5", &offset);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(s.error_message().find("1.5"), std::string::npos);
}

TEST(PageTokenParse, TrailingGarbageReturnsInvalidArgument) {
  // "42xyz" — leading digits parse but trailing chars don't.
  int offset = -1;
  auto s = evgrpc::ParsePageToken("42xyz", &offset);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(PageTokenParse, NegativeNumberClampsToZero) {
  // Matches existing service behavior (`if (offset < 0) offset = 0`).
  // Negative tokens are user-friendly defaults, not errors.
  int offset = -1;
  auto s = evgrpc::ParsePageToken("-5", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 0);
}

TEST(PageTokenParse, NegativeLargeNumberClampsToZero) {
  int offset = -1;
  auto s = evgrpc::ParsePageToken("-999999", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 0);
}

TEST(PageTokenParse, OverflowReturnsInvalidArgument) {
  // Larger than INT_MAX → out of int range.
  int offset = -1;
  auto s = evgrpc::ParsePageToken("99999999999999999", &offset);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(s.error_message().find("out of range"), std::string::npos);
}

TEST(PageTokenParse, IntMaxPlusOneOverflowReturnsInvalidArgument) {
  // INT_MAX + 1 would overflow int; strtol reports ERANGE.
  std::string tok = std::to_string(static_cast<long long>(INT_MAX) + 1);
  int offset = -1;
  auto s = evgrpc::ParsePageToken(tok, &offset);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(PageTokenParse, IntMaxIsAccepted) {
  // INT_MAX itself fits in int — accepted at the boundary.
  int offset = -1;
  auto s = evgrpc::ParsePageToken(std::to_string(INT_MAX), &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, INT_MAX);
}

TEST(PageTokenParse, PlusSignIsAccepted) {
  // strtol accepts a leading + sign. "+42" → 42.
  // The page_token is server-generated (always a bare integer),
  // but accepting +42 is harmless and avoids surprising users.
  int offset = -1;
  auto s = evgrpc::ParsePageToken("+42", &offset);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(offset, 42);
}

}  // namespace