#pragma once

#include "CompanionRecords.h"
#include "platform/Clock.h"

namespace companion {
// A plausible RTC is device evidence; it does not establish distributed Trusted time.
inline bool readTintaMutationClock(const tinta::platform::Clock& clock, uint32_t& studyDay, uint64_t& timestamp,
                                   ClockQuality& quality) {
  if (!clock.trusted()) return false;
  uint64_t utc = 0;
  ClockQuality evidence = ClockQuality::Unknown;
  if (clock.hasTimeOfDay()) {
    if (!clock.unixUtc(utc)) return false;
    evidence = ClockQuality::Device;
  }
  const auto day = clock.today();
  studyDay = day;
  timestamp = utc;
  quality = evidence;
  return true;
}
}  // namespace companion
