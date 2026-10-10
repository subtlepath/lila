#pragma once

#include "HalUnboundCourseLearnerInspection.h"
#include "HalUnboundCourseLessonMapping.h"
#include "HalUnboundCoursePackReader.h"

namespace companion {
// Caller excludes writers and holds the native migration operation lease. This
// combines payload/frozen-profile evidence; replay/publication authority is separate.
inline bool mapUnboundCourseBoundLessons(HalUnboundCourseLearnerInspection& learner,
                                         HalUnboundCoursePackReader& original, HalUnboundCoursePackReader& installed,
                                         const UnboundCourseMigrationIntent& intent, std::span<uint8_t> scratch,
                                         TintaLegacyLessonMapping& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Bound unbound-course lesson mapping refused");
    return false;
  };
  if (!permitted || !permitted(context) || &original == &installed ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &intent, sizeof(intent)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto* evidence = learner.report(intent.request);
  if (!evidence || !original.recheck(intent, UnboundPackRole::Original) ||
      !installed.recheck(intent, UnboundPackRole::Installed))
    return failure();
  const auto profile = evidence->profile;
  auto oldLoan = original.borrowed(intent, UnboundPackRole::Original);
  auto newLoan = installed.borrowed(intent, UnboundPackRole::Installed);
  TintaLegacyLessonMapping result;
  if (!oldLoan || !newLoan ||
      !mapUnboundCourseLessons(profile, *oldLoan.pack, *newLoan.pack, scratch, result, permitted, context) ||
      !original.recheck(intent, UnboundPackRole::Original) || !installed.recheck(intent, UnboundPackRole::Installed) ||
      !learner.report(intent.request) || !permitted(context))
    return failure();
  output = result;
  return true;
}
}  // namespace companion
