#pragma once

#include <Logging.h>

#include "CompanionTintaPreferences.h"

namespace companion {
// Caller has resolved causal conflicts, verified ownership, and excludes profile
// writers. Reconfigure App once after a successful batch, outside rendering.
inline bool persistResolvedTintaPreferences(tinta::core::StateStore& store, tinta::core::Profile& profile,
                                            std::span<const std::span<const uint8_t>> bodies) {
  const auto error = [](const char* reason) {
    LOG_ERR("COMPANION", "Tinta preference application failed: %s", reason);
    return false;
  };
  if (!store.available() || bodies.size() > 9) return error("storage or batch limit");
  auto candidate = profile;
  uint16_t seen = 0;
  for (const auto bytes : bodies) {
    if (bytes.size() < 3 || bytes[2] < 32 || bytes[2] > 40) return error("preference key");
    const auto bit = static_cast<uint16_t>(1U << (bytes[2] - 32));
    if ((seen & bit) || !applyTintaPreference(bytes, candidate)) return error("duplicate or invalid body");
    seen |= bit;
  }
  if (candidate.newPerDay == profile.newPerDay && candidate.reviewCap == profile.reviewCap &&
      candidate.retentionPermille == profile.retentionPermille && candidate.maxInterval == profile.maxInterval &&
      candidate.sessionSize == profile.sessionSize && candidate.textSize == profile.textSize &&
      candidate.uiLanguage == profile.uiLanguage && candidate.showVulgar == profile.showVulgar &&
      candidate.typedAnswers == profile.typedAnswers)
    return true;
  if (!candidate.save(store)) return error("profile replacement");
  profile = candidate;
  return true;
}
}  // namespace companion
