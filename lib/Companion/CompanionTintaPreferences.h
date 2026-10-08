#pragma once

#include "CompanionPreferenceBody.h"
#include "../Tinta/src/core/profile/Profile.h"

namespace companion {
// Caller establishes causal resolution and authority before applying a preference.
// Persistence and runtime reconfiguration happen once after the resolved batch.
inline bool applyTintaPreference(std::span<const uint8_t> bytes, tinta::core::Profile& profile) {
  PreferenceBodyView value;
  if (!decodePreferenceBody(bytes, value) || value.type != 1 || value.key < 32 || value.key > 40) return false;
  switch (value.key) {
    case 32:
      profile.newPerDay = static_cast<uint16_t>(value.integer);
      break;
    case 33:
      profile.reviewCap = static_cast<uint16_t>(value.integer);
      break;
    case 34:
      profile.retentionPermille = static_cast<uint16_t>(value.integer);
      break;
    case 35:
      profile.maxInterval = static_cast<uint16_t>(value.integer);
      break;
    case 36:
      profile.sessionSize = static_cast<uint16_t>(value.integer);
      break;
    case 37:
      profile.textSize = static_cast<tinta::core::TextSize>(value.integer);
      break;
    case 38:
      profile.uiLanguage = static_cast<tinta::core::UiLanguage>(value.integer);
      break;
    case 39:
      profile.showVulgar = value.integer != 0;
      break;
    case 40:
      profile.typedAnswers = value.integer != 0;
      break;
    default:
      return false;
  }
  return true;
}
inline size_t encodeTintaPreference(const tinta::core::Profile& profile, uint8_t key, std::span<uint8_t> output) {
  if (output.size() < 8) return 0;
  int32_t value = 0;
  switch (key) {
    case 32:
      value = profile.newPerDay;
      break;
    case 33:
      value = profile.reviewCap;
      break;
    case 34:
      value = profile.retentionPermille;
      break;
    case 35:
      value = profile.maxInterval;
      break;
    case 36:
      value = profile.sessionSize;
      break;
    case 37:
      value = static_cast<uint8_t>(profile.textSize);
      break;
    case 38:
      value = static_cast<uint8_t>(profile.uiLanguage);
      break;
    case 39:
      value = profile.showVulgar;
      break;
    case 40:
      value = profile.typedAnswers;
      break;
    default:
      return 0;
  }
  if (!validPreferenceInteger(key, value)) return 0;
  std::array<uint8_t, 8> bytes{1, static_cast<uint8_t>(EventKind::Preference), key, 1};
  const auto number = static_cast<uint32_t>(value);
  for (unsigned at = 0; at < 4; ++at) bytes[4 + at] = static_cast<uint8_t>(number >> (8 * at));
  std::copy(bytes.begin(), bytes.end(), output.begin());
  return bytes.size();
}
}  // namespace companion
