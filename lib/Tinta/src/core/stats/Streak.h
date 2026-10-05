#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/stats/DayLog.h"

namespace tinta::core {

struct StreakInfo {
  uint16_t days = 0;          // consecutive study days
  bool studiedToday = false;  // false: the streak ends yesterday and is still alive
};

// The current streak from DayLog: consecutive study days ending today, or
// ending yesterday when today has no study yet.
//
// Only days up to `today` count. On the X4 the day is whatever the learner
// confirmed: a date set too far ahead and then corrected leaves records in
// the "future", which are ignored rather than breaking or inflating the
// streak; a jump forward over missed days ends the streak, as it should; a
// "Next day" after several days away cannot be told apart from a real next
// day (PLAN.md 6.8).
bool currentStreak(DayLog& log, DayNumber today, StreakInfo& out);

}  // namespace tinta::core
