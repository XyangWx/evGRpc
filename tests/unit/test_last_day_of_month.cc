#include <gtest/gtest.h>
#include "util/last_day_of_month.h"

using evgrpc::LastDayOfMonth;

TEST(LastDayOfMonth, CommonMonths) {
  EXPECT_EQ(LastDayOfMonth(2026, 1), 31);
  EXPECT_EQ(LastDayOfMonth(2026, 3), 31);
  EXPECT_EQ(LastDayOfMonth(2026, 4), 30);
  EXPECT_EQ(LastDayOfMonth(2026, 5), 31);
  EXPECT_EQ(LastDayOfMonth(2026, 6), 30);
  EXPECT_EQ(LastDayOfMonth(2026, 9), 30);
  EXPECT_EQ(LastDayOfMonth(2026, 11), 30);
  EXPECT_EQ(LastDayOfMonth(2026, 12), 31);
}

TEST(LastDayOfMonth, FebruaryNonLeap) {
  EXPECT_EQ(LastDayOfMonth(2026, 2), 28);   // 2026 not divisible by 4
  EXPECT_EQ(LastDayOfMonth(2025, 2), 28);
  EXPECT_EQ(LastDayOfMonth(2100, 2), 28);   // century non-leap
}

TEST(LastDayOfMonth, FebruaryLeap) {
  EXPECT_EQ(LastDayOfMonth(2024, 2), 29);   // divisible by 4
  EXPECT_EQ(LastDayOfMonth(2000, 2), 29);   // divisible by 400
  EXPECT_EQ(LastDayOfMonth(2020, 2), 29);
}