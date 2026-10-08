#pragma once

#include <Logging.h>

#include "CompanionTintaPreferenceCapture.h"
#include "HalTintaMutationClock.h"

namespace companion {
// Retain with the learner session; clock/writer and error context outlive this
// callback. Caller initializes from recovered profile before binding App saves.
class HalTintaProfileMutationContext final {
 public:
  HalTintaProfileMutationContext(TintaWriter& writer, const tinta::platform::Clock& clock, uint16_t knownDay,
                                 void* errorContext, void (*reportError)(void*, TintaJournalResult))
      : capture(writer), clock(clock), knownDay(knownDay), errorContext(errorContext), reportError(reportError) {}
  bool initialize(const tinta::core::Profile& profile) { return capture.initialize(profile); }
  static bool callback(void* context, const tinta::core::Profile& profile) {
    return context && static_cast<HalTintaProfileMutationContext*>(context)->persist(profile);
  }
  bool persist(const tinta::core::Profile& profile) {
    if (!reportError) return error(TintaJournalResult::Unavailable);
    bool changed = false;
    if (!capture.hasChanges(profile, changed)) return error(TintaJournalResult::Invalid);
    if (!changed) return true;
    uint32_t day = knownDay;
    uint64_t timestamp = 0;
    ClockQuality quality = ClockQuality::Unknown;
    if (clock.trusted() && !readTintaMutationClock(clock, day, timestamp, quality))
      return error(TintaJournalResult::Invalid);
    const auto result = capture.persist(profile, day, timestamp, quality);
    if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) return error(result);
    knownDay = std::max(knownDay, static_cast<uint16_t>(day));
    return true;
  }

 private:
  bool error(TintaJournalResult result) {
    LOG_ERR("COMPANION", "Tinta profile authority failed: %u", static_cast<unsigned>(result));
    if (reportError) reportError(errorContext, result);
    return false;
  }
  TintaPreferenceCapture capture;
  const tinta::platform::Clock& clock;
  uint16_t knownDay;
  void* errorContext;
  void (*reportError)(void*, TintaJournalResult);
};
}  // namespace companion
