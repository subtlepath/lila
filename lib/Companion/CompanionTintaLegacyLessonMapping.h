#pragma once

#include "CompanionIdentityUniqueness.h"

namespace companion {
struct TintaLegacyLessonMapping {
  uint16_t currentLesson = 0, unlockedThrough = 0;
  uint16_t retainedCompletions = 0, retiredCompletions = 0;
  bool unlockedBoundaryRetired = false;
};
namespace legacy_lesson_mapping {
inline bool find(IdentityKeys& keys, uint32_t identity, uint32_t& position) {
  position = keys.count();
  for (uint32_t index = 0; index < keys.count(); ++index) {
    uint32_t candidate = 0;
    if (!keys.read(index, candidate)) return false;
    if (candidate == identity) {
      position = index;
      break;
    }
  }
  return true;
}
}  // namespace legacy_lesson_mapping
// Immutable original/installed lesson identities; sources bound I/O and yield.
// Retired completions remain evidence, not permission to discard their history.
inline bool mapTintaLegacyLessons(IdentityKeys& original, IdentityKeys& installed, uint16_t completed,
                                  uint16_t unlocked, std::span<uint8_t> scratch, TintaLegacyLessonMapping& output) {
  const auto start = reinterpret_cast<uintptr_t>(scratch.data());
  const auto target = reinterpret_cast<uintptr_t>(&output);
  if ((start <= target ? target - start < scratch.size() : start - target < sizeof(output)) ||
      original.count() > UINT16_MAX || installed.count() > UINT16_MAX || completed > original.count() ||
      (original.count() ? unlocked >= original.count() || (completed < original.count() && completed > unlocked)
                        : completed || unlocked) ||
      !uniqueIdentityKeys(original, scratch) || !uniqueIdentityKeys(installed, scratch))
    return false;
  TintaLegacyLessonMapping result;
  result.currentLesson = static_cast<uint16_t>(installed.count());
  for (uint32_t index = 0; index < installed.count(); ++index) {
    uint32_t identity = 0, previous = 0;
    if (!installed.read(index, identity) || !legacy_lesson_mapping::find(original, identity, previous)) return false;
    if (previous < completed) {
      ++result.retainedCompletions;
      const auto next = static_cast<uint16_t>(index + 1 < installed.count() ? index + 1 : index);
      if (next > result.unlockedThrough) result.unlockedThrough = next;
    } else if (result.currentLesson == installed.count())
      result.currentLesson = static_cast<uint16_t>(index);
  }
  result.retiredCompletions = completed - result.retainedCompletions;
  if (original.count()) {
    uint32_t identity = 0, current = 0;
    if (!original.read(unlocked, identity) || !legacy_lesson_mapping::find(installed, identity, current)) return false;
    result.unlockedBoundaryRetired = current == installed.count();
    if (!result.unlockedBoundaryRetired && current > result.unlockedThrough)
      result.unlockedThrough = static_cast<uint16_t>(current);
  }
  if (result.currentLesson < installed.count() && result.currentLesson > result.unlockedThrough)
    result.unlockedThrough = result.currentLesson;
  output = result;
  return true;
}
}  // namespace companion
