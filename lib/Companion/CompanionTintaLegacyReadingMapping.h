#pragma once

#include "CompanionTintaLegacyStoryIdentity.h"

namespace companion {
enum class LegacyReadingMappingResult { Mapped, OriginalMissing, InstalledMissing, Ambiguous, Invalid, IoError };
struct TintaLegacyReadingMapping {
  uint32_t identity = 0, installedKey = 0;
};
namespace legacy_reading_mapping {
[[gnu::noinline]] inline LegacyStoryIdentityResult originalIdentity(const tinta::core::pack::Pack& pack, uint32_t key,
                                                                    uint32_t& identity) {
  return resolveTintaLegacyStoryKey(pack, key, identity);
}
[[gnu::noinline]] inline LegacyStoryIdentityResult installedKey(const tinta::core::pack::Pack& pack, uint32_t identity,
                                                                uint32_t& key) {
  return resolveTintaStableStoryIdentity(pack, identity, key);
}
inline LegacyReadingMappingResult failure(LegacyStoryIdentityResult result, bool original) {
  switch (result) {
    case LegacyStoryIdentityResult::Missing:
      return original ? LegacyReadingMappingResult::OriginalMissing : LegacyReadingMappingResult::InstalledMissing;
    case LegacyStoryIdentityResult::Ambiguous:
      return LegacyReadingMappingResult::Ambiguous;
    case LegacyStoryIdentityResult::IoError:
      return LegacyReadingMappingResult::IoError;
    default:
      return LegacyReadingMappingResult::Invalid;
  }
}
}  // namespace legacy_reading_mapping
// Caller binds validated immutable packs and bounded/yielding sources. Mapped and
// installed-missing outcomes retain original identity; other outcomes withhold output.
inline LegacyReadingMappingResult mapTintaLegacyReadingKey(const tinta::core::pack::Pack& original,
                                                           const tinta::core::pack::Pack& installed,
                                                           uint32_t originalKey, TintaLegacyReadingMapping& output) {
  if (!original.isOpen() || !installed.isOpen() || !originalKey ||
      original.count(tinta::core::pack::Section::Stor) > UINT16_MAX ||
      installed.count(tinta::core::pack::Section::Stor) > UINT16_MAX)
    return LegacyReadingMappingResult::Invalid;
  TintaLegacyReadingMapping result;
  const auto found = legacy_reading_mapping::originalIdentity(original, originalKey, result.identity);
  if (found != LegacyStoryIdentityResult::Matched) return legacy_reading_mapping::failure(found, true);
  const auto mapped = legacy_reading_mapping::installedKey(installed, result.identity, result.installedKey);
  if (mapped != LegacyStoryIdentityResult::Matched && mapped != LegacyStoryIdentityResult::Missing)
    return legacy_reading_mapping::failure(mapped, false);
  output = result;
  return mapped == LegacyStoryIdentityResult::Matched ? LegacyReadingMappingResult::Mapped
                                                      : LegacyReadingMappingResult::InstalledMissing;
}
}  // namespace companion
