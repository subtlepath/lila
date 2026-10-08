#pragma once

#include "CompanionTintaProfileConfiguration.h"
#include "CompanionTintaProgressJournal.h"
#include "HalTintaMutationClock.h"

namespace companion {
// Retain with the mutation owner; borrowed profile/clock outlive callback execution.
class HalTintaProgressMutationContext {
 public:
  HalTintaProgressMutationContext(const tinta::core::Profile& profile, const tinta::platform::Clock& clock)
      : profile(profile), clock(clock) {}
  tinta::core::ProgressStore::MutationJournal binding(TintaProgressJournal& journal, void* errorContext,
                                                      void (*reportError)(void*, TintaJournalResult)) {
    return journal.binding(this, &configurationCallback, errorContext, reportError, &timestampCallback);
  }
  bool capture(TintaSchedulerConfiguration& output, ClockQuality& quality) {
    captured = false;
    TintaSchedulerConfiguration configuration;
    uint32_t day = 0;
    uint64_t timestamp = 0;
    ClockQuality evidence = ClockQuality::Unknown;
    if (!tintaConfigurationFromProfile(profile, configuration) ||
        !readTintaMutationClock(clock, day, timestamp, evidence))
      return false;
    studyDay = day;
    utc = timestamp;
    clockQuality = evidence;
    output = configuration;
    quality = evidence;
    captured = true;
    return true;
  }
  bool timestamp(const tinta::core::JournalEntry& entry, ClockQuality quality, uint64_t& output) {
    const bool valid = captured && entry.day == studyDay && quality == clockQuality;
    captured = false;
    if (!valid) return false;
    output = utc;
    return true;
  }

 private:
  static bool configurationCallback(void* context, TintaSchedulerConfiguration& configuration, ClockQuality& quality) {
    return static_cast<HalTintaProgressMutationContext*>(context)->capture(configuration, quality);
  }
  static bool timestampCallback(void* context, const tinta::core::JournalEntry& entry, ClockQuality quality,
                                uint64_t& output) {
    return static_cast<HalTintaProgressMutationContext*>(context)->timestamp(entry, quality, output);
  }
  const tinta::core::Profile& profile;
  const tinta::platform::Clock& clock;
  uint32_t studyDay = 0;
  uint64_t utc = 0;
  ClockQuality clockQuality = ClockQuality::Unknown;
  bool captured = false;
};
}  // namespace companion
