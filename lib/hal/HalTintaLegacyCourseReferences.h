#pragma once

#include "../Companion/CompanionCourseUidLookup.h"
#include "CompanionTintaLegacyStoryIdentity.h"
#include "CompanionTintaPackSubjectCatalog.h"
#include "HalTintaLegacyMarkView.h"
#include "HalTintaLegacyProfileValidation.h"

namespace companion {
struct LegacyMarkCatalogReport {
  uint16_t matched = 0, retired = 0;
};
// Pack structure and identity uniqueness must already be validated. Native
// lesson fields are indices into the explicitly confirmed original pack.
inline bool inspectTintaLegacyLessonReferences(const tinta::core::Profile& profile, const tinta::core::pack::Pack& pack,
                                               bool (*permitted)(void*) = nullptr, void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy lesson references invalid: %s", reason);
    return false;
  };
  if (!pack.isOpen() || (permitted && !permitted(context))) return failure("pack or cancellation");
  TintaPackSubjectKeys lessons(pack, false);
  const auto count = lessons.count();
  if (count > UINT16_MAX || profile.currentLesson > count ||
      (count ? profile.unlockedThrough >= count ||
                   (profile.currentLesson < count && profile.currentLesson > profile.unlockedThrough)
             : profile.currentLesson || profile.unlockedThrough))
    return failure("lesson range");
  for (uint32_t index = 0; index < profile.currentLesson; ++index) {
    uint32_t identity = 0;
    if ((permitted && !permitted(context)) || !lessons.read(index, identity)) return failure("lesson identity");
  }
  if (permitted && !permitted(context)) return failure("cancelled");
  return true;
}
// Missing retired references remain evidence; this report alone cannot prove
// historical meaning or compatible retirement. No mark or learner state changes.
inline bool inspectTintaLegacyMarkReferences(HalTintaLegacyMarkView& marks, CourseUidLookup& items,
                                             const tinta::core::pack::Pack& pack, bool readings,
                                             LegacyMarkCatalogReport& output, bool (*permitted)(void*) = nullptr,
                                             void* context = nullptr) {
  const auto failure = []([[maybe_unused]] const char* reason) {
    LOG_ERR("COMPANION", "Legacy mark references invalid: %s", reason);
    return false;
  };
  if (!pack.isOpen() || !items.valid() || (permitted && !permitted(context))) return failure("arguments");
  uint16_t count = 0;
  if (!marks.entryCount(count)) return failure("marks unavailable");
  LegacyMarkCatalogReport result;
  for (uint16_t i = 0; i < count; ++i) {
    uint32_t key = 0;
    if ((permitted && !permitted(context)) || !marks.identityAt(i, key)) return failure("mark read");
    if (readings) {
      uint32_t identity = 0;
      const auto resolved = resolveTintaLegacyStoryKey(pack, key, identity);
      if (resolved == LegacyStoryIdentityResult::Matched)
        ++result.matched;
      else if (resolved == LegacyStoryIdentityResult::Missing)
        ++result.retired;
      else
        return failure("reading ambiguous or unreadable");
    } else {
      if (key == UINT32_MAX) return failure("reserved item UID");
      int32_t index = -1;
      if (!items.find(key, index)) return failure("item lookup");
      if (index < 0)
        ++result.retired;
      else
        ++result.matched;
    }
  }
  if (!items.valid() || (permitted && !permitted(context))) return failure("cancelled or source failed");
  output = result;
  return true;
}
}  // namespace companion
