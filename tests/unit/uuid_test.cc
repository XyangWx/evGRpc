#include <gtest/gtest.h>
#include "util/uuid.h"

TEST(UuidTest, GeneratesValidUuidV4Shape) {
  auto id = evgrpc::NewUuid();
  // RFC 4122 UUID: 8-4-4-4-12 hex chars + 4 dashes = 36 chars total.
  EXPECT_EQ(id.size(), 36u);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[13], '-');
  EXPECT_EQ(id[14], '4');  // version nibble for UUIDv4
  EXPECT_EQ(id[18], '-');
  EXPECT_EQ(id[23], '-');
  // Hex-only body (excluding dashes):
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    char c = id[i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
        << "non-hex at index " << i << ": '" << c << "' in '" << id << "'";
  }
}

TEST(UuidTest, EachCallReturnsUniqueValue) {
  auto a = evgrpc::NewUuid();
  auto b = evgrpc::NewUuid();
  EXPECT_NE(a, b);
}