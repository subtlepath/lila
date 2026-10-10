#pragma once

#include "CompanionCourseItemIdentities.h"
#include "HalTintaLegacyItemCatalogValidation.h"
#include "HalUnboundCourseReviewedFile.h"

namespace companion {
struct UnboundCourseItemReport {
  LegacyItemCatalogReport catalog;
  uint32_t committedRecords = 0;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool beginItemCatalog(CourseUidLookup& catalog) { return catalog.begin(); }
[[gnu::noinline]] inline bool readItemHistory(tinta::core::pack::PackSource& source,
                                              tinta::core::pack::DirEntry& identities, bool& present) {
  return readCourseItemIdentityTable(source, identities, present);
}
[[gnu::noinline]] inline bool inspectItemCatalogFile(HalFile& file, CourseUidLookup& catalog,
                                                     std::span<uint8_t> scratch, LegacyItemCatalogReport& report,
                                                     bool (*permitted)(void*), void* context) {
  return inspectTintaLegacyItemCatalog(file, catalog, scratch, report, permitted, context);
}
[[gnu::noinline]] inline bool inspectItemReferences(HalFile& file, CourseUidLookup& catalog,
                                                    const tinta::core::pack::DirEntry& identities, bool hasIdentities,
                                                    std::span<uint8_t> scratch, uint32_t& committed,
                                                    bool (*permitted)(void*), void* context) {
  HalTintaLegacyItemView items(file);
  if (!items.begin(scratch, permitted, context) || !items.header()) return false;
  committed = items.header()->journalCount;
  const auto count = items.count();
  for (uint32_t index = 0; index < count; ++index) {
    tinta::core::ItemState item;
    if (!items.record(index, item) || !permitted(context)) return false;
    if (item.uid != UINT32_MAX) {
      int32_t activeIndex = -1;
      if (!item.uid || !catalog.find(item.uid, activeIndex) ||
          (activeIndex < 0 && (!hasIdentities || item.uid > identities.count)))
        return false;
    }
  }
  return catalog.valid() && permitted(context);
}
}  // namespace unbound_course_detail
// Source is the fully validated, confirmed original pack. This inspects immutable
// item evidence only; reviews, profiles, session and provenance need separate checks.
inline bool inspectUnboundCourseItems(HalUnboundCourseReviewedFile& reviewed,
                                      const UnboundCourseMigrationRequest& request,
                                      tinta::core::pack::PackSource& original, std::span<uint8_t> scratch,
                                      UnboundCourseItemReport& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course item inspection refused");
    return false;
  };
  if (!permitted || !permitted(context)) return failure();
  const auto opened = reviewed.open(request, "items.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseItemReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (!permitted(context) || !reviewed.closeReaders()) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  CourseUidLookup catalog(original);
  tinta::core::pack::DirEntry identities{};
  bool hasIdentities = false;
  bool valid =
      file && unbound_course_detail::beginItemCatalog(catalog) &&
      unbound_course_detail::readItemHistory(original, identities, hasIdentities) &&
      unbound_course_detail::inspectItemCatalogFile(*file, catalog, scratch, report.catalog, permitted, context);
  if (valid)
    valid = unbound_course_detail::inspectItemReferences(*file, catalog, identities, hasIdentities, scratch,
                                                         report.committedRecords, permitted, context);
  valid = valid && catalog.valid() && permitted(context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion
