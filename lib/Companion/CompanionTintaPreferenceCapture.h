#pragma once

#include "CompanionTintaPreferences.h"
#include "CompanionTintaWriter.h"

namespace companion {
// Retain with the learner session. Initialize from recovered profile before local
// editing. A failed changed batch requires authority recovery before profile save.
class TintaPreferenceCapture final {
 public:
  explicit TintaPreferenceCapture(TintaWriter& writer) : writer(writer) {}
  bool initialize(const tinta::core::Profile& profile) {
    if (initialized) return false;
    ready = false;
    if (!encode(profile)) return false;
    previous = candidate;
    initialized = true;
    ready = true;
    return true;
  }
  bool hasChanges(const tinta::core::Profile& profile, bool& output) {
    if (!ready || !writer.available() || !encode(profile)) return false;
    bool changed = false;
    for (size_t at = 0; at < candidate.size(); ++at) changed |= candidate[at] != previous[at];
    output = changed;
    return true;
  }
  TintaJournalResult persist(const tinta::core::Profile& profile, uint32_t day, uint64_t timestamp,
                             ClockQuality quality) {
    if (!ready || !writer.available()) return TintaJournalResult::Unavailable;
    if (!encode(profile)) return TintaJournalResult::Invalid;
    for (size_t at = 0; at < candidate.size(); ++at) {
      if (previous[at] == candidate[at]) continue;
      const auto result = writer.recordPreference(candidate[at], day, timestamp, quality);
      if (result != TintaJournalResult::Ok && result != TintaJournalResult::Duplicate) {
        ready = false;
        writer.stop();
        return result;
      }
      previous[at] = candidate[at];
    }
    return TintaJournalResult::Ok;
  }

 private:
  bool encode(const tinta::core::Profile& profile) {
    for (size_t at = 0; at < candidate.size(); ++at)
      if (encodeTintaPreference(profile, static_cast<uint8_t>(32 + at), candidate[at]) != candidate[at].size())
        return false;
    return true;
  }
  TintaWriter& writer;
  std::array<std::array<uint8_t, 8>, 9> previous{}, candidate{};
  bool ready = false, initialized = false;
};
}  // namespace companion
