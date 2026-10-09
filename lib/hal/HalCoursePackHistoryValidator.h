#pragma once

#if LILA_TINTA

#include "CompanionStoredCourseContinuity.h"
#include "HalCoursePackHistory.h"
#include "HalCourseValidation.h"
#include "HalHistoricalCourseHistory.h"

namespace companion {
// Caller owns the parser off stack and excludes writers for the complete visit.
class HalCoursePackHistoryValidator final {
 public:
  HalCoursePackHistoryValidator(TransferStorage& storage, tinta::core::pack::Pack& parser, std::span<uint8_t> scratch,
                                HalCoursePackHistory* bridges = nullptr,
                                HalHistoricalCourseHistory* historicalBridges = nullptr)
      : storage(storage), parser(parser), scratch(scratch), bridges(bridges), historicalBridges(historicalBridges) {}
  CourseHistoryResult validate(HalCoursePackHistory& history, const ContentManifest& manifest, const char* path) {
    if (visiting || !path || !validCourseBinding(manifest) || manifest.formatVersion != 1)
      return CourseHistoryResult::Invalid;
    auto result = CourseHistoryResult::Corrupt;
    if (begin(manifest, path)) result = history.visit(manifest.logicalIdentity, visit, this);
    end();
    return result;
  }
  HistoricalCourseHistoryResult validate(HalHistoricalCourseHistory& history, const ContentManifest& manifest,
                                         const char* path) {
    if (visiting || !path || !validCourseBinding(manifest) || manifest.formatVersion != 1)
      return HistoricalCourseHistoryResult::Invalid;
    auto result = HistoricalCourseHistoryResult::Corrupt;
    if (begin(manifest, path)) result = history.visit(visit, this);
    end();
    return result;
  }

 private:
  TransferStorage& storage;
  tinta::core::pack::Pack& parser;
  std::span<uint8_t> scratch;
  HalCoursePackHistory* bridges;
  HalHistoricalCourseHistory* historicalBridges;
  const char* legacyPath = nullptr;
  bool foundBridge = false;
  const ContentManifest* candidate = nullptr;
  const char* candidatePath = nullptr;
  char locale[9]{};
  bool visiting = false;
  bool begin(const ContentManifest& manifest, const char* path) {
    visiting = true;
    candidate = &manifest;
    candidatePath = path;
    CourseCandidateDetails details;
    if (!storage.verify(path, manifest.length, manifest.contentHash, scratch) ||
        !validateStagedCourse(path, parser, scratch, details) || details.major != manifest.formatVersion)
      return false;
    std::memcpy(locale, details.locale, sizeof(locale));
    return true;
  }
  void end() {
    candidate = nullptr;
    candidatePath = nullptr;
    legacyPath = nullptr;
    visiting = false;
  }
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
    if (continuity != CourseItemContinuity::MissingHistory) return false;
    if (sameLegacyCourseRecords(self.storage, path, self.candidatePath, self.scratch, yield)) return true;
    if (!self.bridges && !self.historicalBridges) return false;
    self.legacyPath = path;
    self.foundBridge = false;
    bool valid = true;
    if (self.bridges) {
      const auto result = self.bridges->visit(self.candidate->logicalIdentity, bridge, &self);
      valid = result == CourseHistoryResult::Ok || result == CourseHistoryResult::MissingBaseline;
    }
    if (valid && self.historicalBridges && !self.foundBridge)
      valid = self.historicalBridges->visit(bridge, &self) == HistoricalCourseHistoryResult::Ok;
    self.legacyPath = nullptr;
    return valid && self.foundBridge;
  }
  static bool bridge(void* context, const ContentManifest& manifest, const char* path) {
    auto& self = *static_cast<HalCoursePackHistoryValidator*>(context);
    if (!self.candidate || !self.legacyPath || manifest.logicalIdentity != self.candidate->logicalIdentity ||
        manifest.formatVersion != self.candidate->formatVersion)
      return false;
    CourseCandidateDetails details;
    if (!validateStagedCourse(path, self.parser, self.scratch, details) || details.major != manifest.formatVersion)
      return false;
    for (unsigned at = 0; at < sizeof(self.locale); ++at)
      if (lower(self.locale[at]) != lower(details.locale[at])) return false;
    const auto continuity =
        compareStoredCourseItemIdentities(self.storage, path, self.candidatePath, yield, self.scratch);
    if (continuity == CourseItemContinuity::MissingHistory) return true;
    if (continuity != CourseItemContinuity::Compatible) return false;
    if (sameLegacyCourseRecords(self.storage, self.legacyPath, path, self.scratch, yield)) self.foundBridge = true;
    return true;
  }
};
}  // namespace companion

#endif  // LILA_TINTA
