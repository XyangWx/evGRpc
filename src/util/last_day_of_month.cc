#include "util/last_day_of_month.h"

namespace evgrpc {
int LastDayOfMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  int days = kDays[month - 1];
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (leap) days = 29;
  }
  return days;
}
}  // namespace evgrpc