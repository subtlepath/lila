#pragma once

#include "CompanionTintaLegacyReadingMapping.h"
#include "HalUnboundCourseBoundLessonMapping.h"

namespace companion {
struct UnboundCourseReadingMappingReport {
  uint16_t mapped = 0, installedMissing = 0, originalMissing = 0;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool mapBoundReadingFile(HalFile& file, const tinta::core::pack::Pack& original,
                                                  const tinta::core::pack::Pack& installed, std::span<uint8_t> scratch,
                                                  uint16_t expected, UnboundCourseReadingMappingReport& report,
                                                  bool (*permitted)(void*), void* context) {
  HalTintaLegacyMarkView marks(file);
  uint16_t count = 0;
  if (!marks.begin(scratch, permitted, context) || !marks.entryCount(count) || count != expected) return false;
  for (uint16_t index = 0; index < count; ++index) {
    uint32_t key = 0;
    TintaLegacyReadingMapping mapped;
    if (!permitted(context) || !marks.identityAt(index, key)) return false;
    switch (mapTintaLegacyReadingKey(original, installed, key, mapped)) {
      case LegacyReadingMappingResult::Mapped:
        ++report.mapped;
        break;
      case LegacyReadingMappingResult::InstalledMissing:
        ++report.installedMissing;
        break;
      case LegacyReadingMappingResult::OriginalMissing:
        ++report.originalMissing;
        break;
      default:
        return false;
    }
  }
  return permitted(context);
}
}  // namespace unbound_course_detail
// Native parent freezes intent/owners and excludes writers for the operation.
// Missing identities remain evidence requiring policy, never discarded history.
inline bool mapUnboundCourseBoundReadings(HalUnboundCourseLearnerInspection& learner,
                                          HalUnboundCourseReviewedFile& reviewed, HalUnboundCoursePackReader& original,
                                          HalUnboundCoursePackReader& installed,
                                          const UnboundCourseMigrationIntent& intent, std::span<uint8_t> scratch,
                                          UnboundCourseReadingMappingReport& output, bool (*permitted)(void*),
                                          void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Bound unbound-course reading mapping refused");
    return false;
  };
  if (!permitted || !permitted(context) || &original == &installed ||
      scratch.size() < HalTintaLegacyMarkView::WORKSPACE_SIZE ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &intent, sizeof(intent)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto* evidence = learner.report(intent.request);
  if (!evidence) return failure();
  const bool present = evidence->readings.present;
  const uint16_t count = evidence->readings.catalog.matched + evidence->readings.catalog.retired;
  if (!original.recheck(intent, UnboundPackRole::Original) || !installed.recheck(intent, UnboundPackRole::Installed))
    return failure();
  auto oldLoan = original.borrowed(intent, UnboundPackRole::Original);
  auto newLoan = installed.borrowed(intent, UnboundPackRole::Installed);
  if (!oldLoan || !newLoan) return failure();
  const auto opened = reviewed.open(intent.request, "read.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseReadingMappingReport report;
  bool valid = false;
  if (opened == UnboundReviewedFileResult::Missing)
    valid = !present && !count;
  else {
    auto* file = reviewed.borrowed();
    valid = present && file &&
            unbound_course_detail::mapBoundReadingFile(*file, *oldLoan.pack, *newLoan.pack, scratch, count, report,
                                                       permitted, context);
    report.present = true;
  }
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !original.recheck(intent, UnboundPackRole::Original) ||
      !installed.recheck(intent, UnboundPackRole::Installed) || !learner.report(intent.request) || !permitted(context))
    return failure();
  output = report;
  return true;
}
}  // namespace companion
