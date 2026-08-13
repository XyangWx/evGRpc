#pragma once
namespace evgrpc {
// Returns the last valid day of (year, month). Handles Gregorian leap
// years per the divisible-by-4 / -100 / -400 rule.
int LastDayOfMonth(int year, int month);
}  // namespace evgrpc