#pragma once

#include "CompanionTintaBody.h"
#include "core/profile/Profile.h"

namespace companion {
// Read at each mutation after App::profileChanged applies scheduler settings.
inline bool tintaConfigurationFromProfile(const tinta::core::Profile& profile, TintaSchedulerConfiguration& output) {
  if (profile.retentionPermille < 700 || profile.retentionPermille > 970 || profile.maxInterval == 0 ||
      profile.maxInterval > 36500)
    return false;
  output.retentionBasisPoints = static_cast<uint16_t>(profile.retentionPermille * 10);
  output.maximumInterval = profile.maxInterval;
  return true;
}
}  // namespace companion
