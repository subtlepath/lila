#pragma once

#if LILA_TINTA

#include "CompanionStoredCourseContinuity.h"
#include "HalCoursePackHistory.h"
#include "HalCourseValidation.h"

namespace companion {
// Caller owns the parser off stack and excludes writers for the complete visit.
class HalCoursePackHistoryValidator final {
 public:
  HalCoursePackHistoryValidator(TransferStorage& storage, tinta::core::pack::Pack& parser, std::span<uint8_t> scratch)
      : storage(storage), parser(parser), scratch(scratch) {}
  CourseHistoryResult validate(HalCoursePackHistory& history, const ContentManifest& manifest, const char* path) {
    if (visiting || !path || !validCourseBinding(manifest) || manifest.formatVersion != 1)
      return CourseHistoryResult::Invalid;
    visiting = true;
    candidate = &manifest;
    candidatePath = path;
    CourseCandidateDetails details;
    auto result = CourseHistoryResult::Corrupt;
    if (storage.verify(path, manifest.length, manifest.contentHash, scratch) &&
        validateStagedCourse(path, parser, scratch, details) && details.major == manifest.formatVersion) {
      std::memcpy(locale, details.locale, sizeof(locale));
      result = history.visit(manifest.logicalIdentity, visit, this);
    }
    candidate = nullptr;
    candidatePath = nullptr;
    visiting = false;
    return result;
  }

 private:
  TransferStorage& storage;
  tinta::core::pack::Pack& parser;
  std::span<uint8_t> scratch;
  const ContentManifest* candidate = nullptr;
  const char* candidatePath = nullptr;
  char locale[9]{};
  bool visiting = false;
  static unsigned char lower(unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; }
  static void yield() { vTaskDelay(1); }
  static bool visit(void* context, const ContentManifest& manifest, const char* path) {
    auto& self = *static_cast<HalCoursePackHistoryValidator*>(context);
    if (!self.candidate || manifest.logicalIdentity != self.candidate->logicalIdentity ||
        manifest.formatVersion != self.candidate->formatVersion)
      return false;
    CourseCandidateDetails details;
    if (!validateStagedCourse(path, self.parser, self.scratch, details) || details.major != manifest.formatVersion)
      return false;
    for (unsigned at = 0; at < sizeof(self.locale); ++at)
      if (lower(self.locale[at]) != lower(details.locale[at])) return false;
    const auto continuity =
        compareStoredCourseItemIdentities(self.storage, path, self.candidatePath, yield, self.scratch);
    if (continuity == CourseItemContinuity::Compatible) return true;
    return continuity == CourseItemContinuity::MissingHistory &&
           sameLegacyCourseRecords(self.storage, path, self.candidatePath, self.scratch, yield);
  }
};
}  // namespace companion

#endif  // LILA_TINTA
