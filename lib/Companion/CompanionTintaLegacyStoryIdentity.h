#pragma once

#include "CompanionTintaPackStoryIdentity.h"

namespace companion {
enum class LegacyStoryIdentityResult { Matched, Missing, Ambiguous, Invalid, IoError };
inline bool tintaLegacyStoryKey(const tinta::core::pack::Pack& pack, const tinta::core::pack::Story& story,
                                uint32_t& output) {
  uint32_t hash = 2166136261u;
  const auto visit = [](void* context, const uint8_t* bytes, uint32_t size) {
    auto& hash = *static_cast<uint32_t*>(context);
    for (uint32_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return true;
  };
  if (!pack.visitStr(story.title, &hash, visit)) return false;
  output = hash ? hash : 1u;
  return true;
}
// Caller validates the immutable pack and stable identity uniqueness. No string arena or heap is used.
inline LegacyStoryIdentityResult resolveTintaLegacyStoryKey(const tinta::core::pack::Pack& pack, uint32_t legacyKey,
                                                            uint32_t& output) {
  namespace pk = tinta::core::pack;
  if (!pack.isOpen() || !legacyKey || pack.count(pk::Section::Stor) > UINT16_MAX)
    return LegacyStoryIdentityResult::Invalid;
  uint32_t identity = 0;
  bool found = false;
  for (uint32_t index = 0; index < pack.count(pk::Section::Stor); ++index) {
    pk::Story story;
    if (!pack.story(static_cast<uint16_t>(index), story)) return LegacyStoryIdentityResult::IoError;
    if (!story.title || static_cast<uint8_t>(story.kind) > 1) return LegacyStoryIdentityResult::Invalid;
    uint32_t key = 0;
    if (!tintaLegacyStoryKey(pack, story, key)) return LegacyStoryIdentityResult::IoError;
    if (key != legacyKey) continue;
    if (found) return LegacyStoryIdentityResult::Ambiguous;
    if (!tintaPackStoryIdentity(pack, story, identity)) return LegacyStoryIdentityResult::IoError;
    found = true;
  }
  if (!found) return LegacyStoryIdentityResult::Missing;
  output = identity;
  return LegacyStoryIdentityResult::Matched;
}
// A native title key must identify exactly the requested stable story.
inline LegacyStoryIdentityResult resolveTintaStableStoryIdentity(const tinta::core::pack::Pack& pack,
                                                                 uint32_t stableIdentity, uint32_t& output) {
  namespace pk = tinta::core::pack;
  if (!pack.isOpen() || !stableIdentity || stableIdentity == UINT32_MAX || pack.count(pk::Section::Stor) > UINT16_MAX)
    return LegacyStoryIdentityResult::Invalid;
  bool found = false;
  uint32_t key = 0;
  for (uint32_t index = 0; index < pack.count(pk::Section::Stor); ++index) {
    pk::Story story;
    if (!pack.story(static_cast<uint16_t>(index), story)) return LegacyStoryIdentityResult::IoError;
    if (!story.title || static_cast<uint8_t>(story.kind) > 1) return LegacyStoryIdentityResult::Invalid;
    uint32_t identity = 0;
    if (!tintaPackStoryIdentity(pack, story, identity)) return LegacyStoryIdentityResult::IoError;
    if (identity != stableIdentity) continue;
    if (found) return LegacyStoryIdentityResult::Ambiguous;
    if (!tintaLegacyStoryKey(pack, story, key)) return LegacyStoryIdentityResult::IoError;
    found = true;
  }
  if (!found) return LegacyStoryIdentityResult::Missing;
  uint32_t roundTrip = 0;
  const auto result = resolveTintaLegacyStoryKey(pack, key, roundTrip);
  if (result != LegacyStoryIdentityResult::Matched) return result;
  if (roundTrip != stableIdentity) return LegacyStoryIdentityResult::Ambiguous;
  output = key;
  return LegacyStoryIdentityResult::Matched;
}
}  // namespace companion
