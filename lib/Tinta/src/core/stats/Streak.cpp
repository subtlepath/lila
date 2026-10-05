#include "core/stats/Streak.h"

namespace tinta::core {

namespace {

constexpr uint16_t kWindow = 256;  // days per scan of days.bin; 32 bytes of bits

bool bit(const uint8_t* bits, uint32_t i) { return (bits[i / 8] >> (i % 8)) & 1u; }

}  // namespace

bool currentStreak(DayLog& log, DayNumber today, StreakInfo& out) {
  out = StreakInfo();
  uint8_t bits[kWindow / 8];

  // Walk back from `day`, one window of days per scan of the log, until a
  // day without study. Streaks longer than a window cost another scan.
  uint32_t day = today;
  bool first = true;
  for (;;) {
    const uint32_t start = day + 1 >= kWindow ? day + 1 - kWindow : 0;
    const uint16_t count = static_cast<uint16_t>(day - start + 1);
    if (!log.studyDays(static_cast<DayNumber>(start), count, bits)) return false;
    uint32_t i = day - start;
    if (first) {
      first = false;
      out.studiedToday = bit(bits, i);
      if (!out.studiedToday) {
        if (i == 0) return true;  // day 0 has no yesterday
        --i;
      }
    }
    for (;; --i) {
      if (!bit(bits, i)) return true;
      if (out.days < 0xFFFF) ++out.days;
      if (i == 0) break;
    }
    if (start == 0) return true;
    day = start - 1;
  }
}

}  // namespace tinta::core
