// modules:

#include "core/Date.h"

#include "check.h"

using namespace tinta::core;

namespace {

static_assert(date::dayNumber({2024, 1, 1}) == 0, "epoch");
static_assert(date::dayNumber({2024, 12, 31}) == 365, "2024 is a leap year");
static_assert(date::weekday(date::dayNumber({2026, 10, 5})) == 0, "2026-10-05 is a Monday");

void testRoundTrip() {
  for (uint32_t d = 0; d <= 0xFFFF; d += 7) {
    const date::Civil c = date::civil(static_cast<DayNumber>(d));
    CHECK(date::valid(c) || c.year > date::kLastYear);
    CHECK_EQ(date::dayNumber(c), d);
  }
}

void testKnownDates() {
  CHECK_EQ(date::dayNumber({2026, 10, 3}), 1006);
  const date::Civil c = date::civil(1006);
  CHECK_EQ(c.year, 2026);
  CHECK_EQ(c.month, 10);
  CHECK_EQ(c.day, 3);
  CHECK_EQ(date::daysInMonth(2028, 2), 29);
  CHECK_EQ(date::daysInMonth(2100, 2), 28);
  CHECK_EQ(date::daysInMonth(2000, 2), 29);
  CHECK(!date::valid({2025, 2, 29}));
  CHECK(!date::valid({2023, 12, 31}));
}

void testStudyDay() {
  const uint32_t day = 1006;
  const uint32_t midnight = day * 86400u;
  CHECK_EQ(date::studyDay(midnight + 3 * 3600 + 59 * 60, 4), day - 1);  // 03:59 is still yesterday
  CHECK_EQ(date::studyDay(midnight + 4 * 3600, 4), day);
  CHECK_EQ(date::studyDay(midnight + 23 * 3600, 0), day);
  CHECK_EQ(date::studyDay(100, 4), 0);
}

}  // namespace

int main() {
  testRoundTrip();
  testKnownDates();
  testStudyDay();
  return tinta_test::result();
}
