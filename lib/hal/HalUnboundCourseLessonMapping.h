#pragma once

#include "CompanionTintaLegacyLessonMapping.h"
#include "HalUnboundCourseProfileInspection.h"

namespace companion {
namespace unbound_course_detail {
class GuardedLessonKeys final : public IdentityKeys {
 public:
  GuardedLessonKeys(const tinta::core::pack::Pack& pack, bool (*permitted)(void*), void* context)
      : keys(pack, false), permitted(permitted), context(context) {}
  uint32_t count() const override { return keys.count(); }
  bool read(uint32_t index, uint32_t& output) override {
    if (++reads == 32) {
      reads = 0;
      vTaskDelay(1);
    }
    return permitted(context) && admitCompanionHeap() && keys.read(index, output) && permitted(context);
  }

 private:
  TintaPackSubjectKeys keys;
  bool (*permitted)(void*);
  void* context;
  uint8_t reads = 0;
};
[[gnu::noinline]] inline bool inspectMappingProfile(const tinta::core::Profile& profile,
                                                    const tinta::core::pack::Pack& original, bool (*permitted)(void*),
                                                    void* context) {
  return inspectTintaLegacyLessonReferences(profile, original, permitted, context);
}
}  // namespace unbound_course_detail
// Caller binds fully validated immutable original/installed packs and the frozen
// profile report. Mapping is evidence, not authority to publish learner state.
inline bool mapUnboundCourseLessons(const UnboundCourseProfileReport& profile, const tinta::core::pack::Pack& original,
                                    const tinta::core::pack::Pack& installed, std::span<uint8_t> scratch,
                                    TintaLegacyLessonMapping& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course lesson mapping refused");
    return false;
  };
  if (!permitted || !permitted(context) || !admitCompanionHeap() || !original.isOpen() || !installed.isOpen() ||
      original.count(tinta::core::pack::Section::Less) > UINT16_MAX ||
      installed.count(tinta::core::pack::Section::Less) > UINT16_MAX ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &profile, sizeof(profile)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &original, sizeof(original)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &installed, sizeof(installed)))
    return failure();
  unbound_course_detail::GuardedLessonKeys oldKeys(original, permitted, context),
      newKeys(installed, permitted, context);
  TintaLegacyLessonMapping result;
  bool valid = false;
  if (profile.present) {
    valid = (profile.status == tinta::core::Profile::LoadResult::Loaded ||
             profile.status == tinta::core::Profile::LoadResult::Upgraded) &&
            unbound_course_detail::inspectMappingProfile(profile.profile, original, permitted, context) &&
            mapTintaLegacyLessons(oldKeys, newKeys, profile.profile.currentLesson, profile.profile.unlockedThrough,
                                  scratch, result);
  } else {
    valid = uniqueIdentityKeys(oldKeys, scratch) && uniqueIdentityKeys(newKeys, scratch);
  }
  if (!valid || !permitted(context) || !admitCompanionHeap()) return failure();
  output = result;
  return true;
}
}  // namespace companion
