#pragma once

#include "HalTintaLegacyReviewValidation.h"
#include "HalUnboundCourseItemInspection.h"

namespace companion {
struct UnboundCourseReviewReport {
  LegacyReviewReport journal;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool inspectReviewFile(HalFile& file, CourseUidLookup& catalog, uint32_t committed,
                                                std::span<uint8_t> scratch, LegacyReviewReport& report,
                                                bool (*permitted)(void*), void* context) {
  return inspectTintaLegacyReviews(file, catalog, committed, scratch, report, permitted, context);
}
[[gnu::noinline]] inline bool inspectReviewReferences(HalFile& file, CourseUidLookup& catalog,
                                                      const tinta::core::pack::DirEntry& identities, bool hasIdentities,
                                                      uint32_t records, std::span<uint8_t> scratch,
                                                      bool (*permitted)(void*), void* context) {
  if (scratch.size() < 12 || !file.seek64(0)) return false;
  for (uint32_t index = 0; index < records; ++index) {
    if (!permitted(context) || file.read(scratch.data(), 12) != 12) return false;
    const auto uid = binary_record::getU32(scratch.data());
    int32_t activeIndex = -1;
    if (!uid || !catalog.find(uid, activeIndex) || (activeIndex < 0 && (!hasIdentities || uid > identities.count)))
      return false;
  }
  return catalog.valid() && permitted(context);
}
}  // namespace unbound_course_detail
// Source and item report belong to this frozen review and confirmed original pack.
// This validates legacy records/undo syntax; distributed provenance and schedule
// replay correspondence are separate obligations before any learner mutation.
inline bool inspectUnboundCourseReviews(HalUnboundCourseReviewedFile& reviewed,
                                        const UnboundCourseMigrationRequest& request,
                                        tinta::core::pack::PackSource& original, const UnboundCourseItemReport& items,
                                        std::span<uint8_t> scratch, UnboundCourseReviewReport& output,
                                        bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course review inspection refused");
    return false;
  };
  if (!permitted || !permitted(context) || (!items.present && items.committedRecords) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &items, sizeof(items)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto opened = reviewed.open(request, "reviews.log");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseReviewReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (items.committedRecords || !reviewed.closeReaders() || !permitted(context)) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  CourseUidLookup catalog(original);
  tinta::core::pack::DirEntry identities{};
  bool hasIdentities = false;
  bool valid = file && unbound_course_detail::beginItemCatalog(catalog) &&
               unbound_course_detail::readItemHistory(original, identities, hasIdentities) &&
               unbound_course_detail::inspectReviewFile(*file, catalog, items.committedRecords, scratch, report.journal,
                                                        permitted, context);
  if (valid)
    valid = unbound_course_detail::inspectReviewReferences(*file, catalog, identities, hasIdentities,
                                                           report.journal.records, scratch, permitted, context);
  valid = valid && catalog.valid() && permitted(context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion
