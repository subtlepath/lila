#pragma once

#include "HalTintaLegacyCourseReferences.h"
#include "HalUnboundCourseReviewedFile.h"

namespace companion {
struct UnboundCourseProfileReport {
  tinta::core::Profile profile;
  tinta::core::Profile::LoadResult status = tinta::core::Profile::LoadResult::Defaults;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool inspectProfileFile(HalFile& file, UnboundCourseProfileReport& report,
                                                 bool (*permitted)(void*), void* context) {
  return inspectTintaLegacyProfile(file, report.profile, report.status, permitted, context);
}
}  // namespace unbound_course_detail
// Pack is fully validated and belongs to the confirmed original payload. Profile
// repair status remains explicit evidence; no defaults or upgraded bytes are saved.
inline bool inspectUnboundCourseProfile(HalUnboundCourseReviewedFile& reviewed,
                                        const UnboundCourseMigrationRequest& request,
                                        const tinta::core::pack::Pack& original, std::span<uint8_t> scratch,
                                        UnboundCourseProfileReport& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course profile inspection refused");
    return false;
  };
  if (!permitted || !permitted(context) || !original.isOpen() ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto opened = reviewed.open(request, "profile.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseProfileReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (!reviewed.closeReaders() || !permitted(context)) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  const bool valid = file && unbound_course_detail::inspectProfileFile(*file, report, permitted, context) &&
                     inspectTintaLegacyLessonReferences(report.profile, original, permitted, context) &&
                     permitted(context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion
