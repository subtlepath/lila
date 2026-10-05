#pragma once

#include <cstdint>

#include "core/Clock.h"

namespace tinta::core::date {

// Proleptic Gregorian calendar dates and the scheduler's DayNumber (days since
// 2024-01-01). Days-from-civil after Howard Hinnant; constexpr so tables and
// tests can use it at compile time.

struct Civil {
  uint16_t year = 2024;
  uint8_t month = 1;  // 1..12
  uint8_t day = 1;    // 1..31
};

inline constexpr uint16_t kFirstYear = 2024;
// DayNumber is 16-bit: 2024-01-01 + 65535 days is in 2203.
inline constexpr uint16_t kLastYear = 2199;

constexpr bool isLeap(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

constexpr uint8_t daysInMonth(uint16_t year, uint8_t month) {
  constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && isLeap(year) ? 29 : kDays[month - 1];
}

constexpr bool valid(const Civil& c) {
  return c.year >= kFirstYear && c.year <= kLastYear && c.day >= 1 && c.day <= daysInMonth(c.year, c.month);
}

// Days since 1970-01-01.
constexpr int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? static_cast<uint32_t>(-3) : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

constexpr Civil civilFromDays(int32_t z) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t y = static_cast<int32_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  Civil c;
  c.year = static_cast<uint16_t>(y + (m <= 2 ? 1 : 0));
  c.month = static_cast<uint8_t>(m);
  c.day = static_cast<uint8_t>(d);
  return c;
}

inline constexpr int32_t kEpochDays = daysFromCivil(2024, 1, 1);

constexpr DayNumber dayNumber(const Civil& c) {
  return static_cast<DayNumber>(daysFromCivil(c.year, c.month, c.day) - kEpochDays);
}

constexpr Civil civil(DayNumber day) { return civilFromDays(kEpochDays + day); }

// 0 = Monday .. 6 = Sunday (2024-01-01 was a Monday).
constexpr uint8_t weekday(DayNumber day) { return static_cast<uint8_t>(day % 7); }

// The study day for a local time (seconds since 2024-01-01 00:00 local): the
// day changes at `rolloverHour` rather than at midnight, so a late session
// still counts for the evening it started in (PLAN.md 6.8).
constexpr DayNumber studyDay(uint32_t localSeconds, uint8_t rolloverHour) {
  const uint32_t shift = static_cast<uint32_t>(rolloverHour) * 3600u;
  if (localSeconds < shift) return 0;
  const uint32_t day = (localSeconds - shift) / 86400u;
  return static_cast<DayNumber>(day > 0xFFFFu ? 0xFFFFu : day);
}

}  // namespace tinta::core::date
