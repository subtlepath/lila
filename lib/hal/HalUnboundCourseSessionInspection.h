#pragma once

#include "HalTintaLegacySessionValidation.h"
#include "HalUnboundCourseReviewInspection.h"
#include "app/PersistedSessionLimits.h"

namespace companion {
struct UnboundCourseSessionReport {
  LegacySessionReport session;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool inspectSessionSyntax(HalFile& file, CourseUidLookup& catalog,
                                                   const tinta::core::pack::Pack& pack, uint32_t records,
                                                   std::span<uint8_t> scratch, LegacySessionReport& report,
                                                   bool (*permitted)(void*), void* context) {
  return inspectTintaLegacySession(file, catalog, records, pack.count(tinta::core::pack::Section::Less),
                                   pack.count(tinta::core::pack::Section::Phrs), tinta::app::kSessionStackCapacity,
                                   static_cast<uint8_t>(tinta::app::ScreenId::Count), tinta::app::kSessionQueueCapacity,
                                   scratch, report, permitted, context);
}
[[gnu::noinline]] inline bool inspectSessionHistory(HalFile& file, CourseUidLookup& catalog,
                                                    const tinta::core::pack::DirEntry& identities, bool hasIdentities,
                                                    std::span<uint8_t> scratch, bool (*permitted)(void*),
                                                    void* context) {
  const auto length = file.fileSize64();
  if (length > scratch.size()) return false;
  LegacySessionView saved;
  if (!decodeTintaLegacySession(scratch.first(length), tinta::app::kSessionStackCapacity,
                                static_cast<uint8_t>(tinta::app::ScreenId::Count), tinta::app::kSessionQueueCapacity,
                                saved))
    return false;
  for (size_t at = 0; at < saved.entries.size(); at += 5) {
    const auto uid = binary_record::getU32(saved.entries.data() + at);
    int32_t activeIndex = -1;
    if (!permitted(context) || !catalog.find(uid, activeIndex) ||
        (activeIndex < 0 && (!hasIdentities || uid > identities.count)))
      return false;
  }
  return file.fileSize64() == length && catalog.valid() && permitted(context);
}
[[gnu::noinline]] inline bool inspectSessionFile(HalFile& file, tinta::core::pack::PackSource& source,
                                                 const tinta::core::pack::Pack& pack, uint32_t records,
                                                 std::span<uint8_t> scratch, LegacySessionReport& report,
                                                 bool (*permitted)(void*), void* context) {
  CourseUidLookup catalog(source);
  tinta::core::pack::DirEntry identities{};
  bool hasIdentities = false;
  return beginItemCatalog(catalog) && readItemHistory(source, identities, hasIdentities) &&
         inspectSessionSyntax(file, catalog, pack, records, scratch, report, permitted, context) &&
         inspectSessionHistory(file, catalog, identities, hasIdentities, scratch, permitted, context);
}
}  // namespace unbound_course_detail
// Pack/source and item/review reports belong to the same frozen review. Saved
// snapshot and journal-change flags are evidence, not authority to restore a queue.
inline bool inspectUnboundCourseSession(HalUnboundCourseReviewedFile& reviewed,
                                        const UnboundCourseMigrationRequest& request,
                                        tinta::core::pack::PackSource& source, const tinta::core::pack::Pack& pack,
                                        const UnboundCourseItemReport& items, const UnboundCourseReviewReport& reviews,
                                        std::span<uint8_t> scratch, UnboundCourseSessionReport& output,
                                        bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course session inspection refused");
    return false;
  };
  if (!permitted || !permitted(context) || !pack.isOpen() || (!items.present && items.committedRecords) ||
      (!reviews.present && items.committedRecords) ||
      (reviews.present && reviews.journal.records < items.committedRecords) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &items, sizeof(items)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &reviews, sizeof(reviews)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const auto opened = reviewed.open(request, "session.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseSessionReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (!reviewed.closeReaders() || !permitted(context)) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  const auto records = reviews.present ? reviews.journal.records : items.committedRecords;
  const bool valid = file && unbound_course_detail::inspectSessionFile(*file, source, pack, records, scratch,
                                                                       report.session, permitted, context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion
