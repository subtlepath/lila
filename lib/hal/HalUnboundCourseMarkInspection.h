#pragma once

#include "HalTintaLegacyCourseReferences.h"
#include "HalUnboundCourseItemInspection.h"

namespace companion {
enum class UnboundCourseMarkKind { Stars, Readings };
struct UnboundCourseMarkReport {
  LegacyMarkCatalogReport catalog;
  bool present = false;
};
namespace unbound_course_detail {
[[gnu::noinline]] inline bool inspectMarkHistory(HalTintaLegacyMarkView& marks, CourseUidLookup& catalog,
                                                 const tinta::core::pack::DirEntry& identities, bool hasIdentities,
                                                 bool (*permitted)(void*), void* context) {
  uint16_t count = 0;
  if (!marks.entryCount(count)) return false;
  for (uint16_t index = 0; index < count; ++index) {
    uint32_t uid = 0;
    int32_t activeIndex = -1;
    if (!permitted(context) || !marks.identityAt(index, uid) || !catalog.find(uid, activeIndex) ||
        (activeIndex < 0 && (!hasIdentities || uid > identities.count)))
      return false;
  }
  return catalog.valid() && permitted(context);
}
[[gnu::noinline]] inline bool inspectMarkFile(HalFile& file, tinta::core::pack::PackSource& source,
                                              const tinta::core::pack::Pack& pack, bool readings,
                                              std::span<uint8_t> scratch, LegacyMarkCatalogReport& output,
                                              bool (*permitted)(void*), void* context) {
  CourseUidLookup catalog(source);
  HalTintaLegacyMarkView marks(file);
  tinta::core::pack::DirEntry identities{};
  bool hasIdentities = false;
  if (!beginItemCatalog(catalog) || !marks.begin(scratch, permitted, context)) return false;
  if (!readings && (!readItemHistory(source, identities, hasIdentities) ||
                    !inspectMarkHistory(marks, catalog, identities, hasIdentities, permitted, context)))
    return false;
  return inspectTintaLegacyMarkReferences(marks, catalog, pack, readings, output, permitted, context);
}
}  // namespace unbound_course_detail
// Source and pack are views of the same fully validated confirmed original.
// Current mark membership is evidence; distributed mutation ancestry is separate.
inline bool inspectUnboundCourseMarks(HalUnboundCourseReviewedFile& reviewed,
                                      const UnboundCourseMigrationRequest& request,
                                      tinta::core::pack::PackSource& source, const tinta::core::pack::Pack& pack,
                                      UnboundCourseMarkKind kind, std::span<uint8_t> scratch,
                                      UnboundCourseMarkReport& output, bool (*permitted)(void*), void* context) {
  const auto failure = [] {
    LOG_ERR("COMPANION", "Unbound course mark inspection refused");
    return false;
  };
  if (!permitted || !permitted(context) || !pack.isOpen() ||
      (kind != UnboundCourseMarkKind::Stars && kind != UnboundCourseMarkKind::Readings) ||
      scratch.size() < HalTintaLegacyMarkView::WORKSPACE_SIZE ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)))
    return failure();
  const bool readings = kind == UnboundCourseMarkKind::Readings;
  const auto opened = reviewed.open(request, readings ? "read.bin" : "starred.bin");
  if (opened != UnboundReviewedFileResult::Present && opened != UnboundReviewedFileResult::Missing) return failure();
  UnboundCourseMarkReport report;
  if (opened == UnboundReviewedFileResult::Missing) {
    if (!reviewed.closeReaders() || !permitted(context)) return failure();
    output = report;
    return true;
  }
  auto* file = reviewed.borrowed();
  const bool valid = file && unbound_course_detail::inspectMarkFile(*file, source, pack, readings, scratch,
                                                                    report.catalog, permitted, context);
  const bool closed = reviewed.closeReaders();
  if (!valid || !closed || !permitted(context)) return failure();
  report.present = true;
  output = report;
  return true;
}
}  // namespace companion
