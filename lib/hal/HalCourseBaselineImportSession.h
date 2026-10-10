#pragma once

#if LILA_TINTA

#include "HalCourseBaselineLearnerInspection.h"
#include "HalCourseBaselineNativeInstaller.h"

namespace companion {
// Retain outside the stack with the borrowed workspace and permission context.
// Detach installer() from transfer storage before releasing this session.
// Permission also proves exclusive workspace ownership: transport queues and
// frame loans must be inactive while the full workspace is used.
class HalCourseBaselineImportSession final {
 public:
  HalCourseBaselineImportSession(const Identity& reader, const Identity& generation, const Identity& owner,
                                 std::span<uint8_t> scratch, CourseBaselineSessionLimits limits,
                                 HalCourseBaselineLearnerInspection::Permission permitted, void* context,
                                 std::span<uint8_t> parentWorkspace)
      : inspection(reader, generation, owner, scratch, parser, limits, permitted, context),
        nativeInstaller(reader, generation, owner, scratch, permitted, context,
                        HalCourseBaselineLearnerInspection::compatible, &inspection),
        parentInstaller(nativeInstaller, parentWorkspace, scratch) {}
  HalCourseBaselineImportSession(const HalCourseBaselineImportSession&) = delete;
  HalCourseBaselineImportSession& operator=(const HalCourseBaselineImportSession&) = delete;
  HalCourseBaselineTransferInstaller* installer() { return &parentInstaller; }

 private:
  class ParentInstaller final : public HalCourseBaselineTransferInstaller {
   public:
    ParentInstaller(HalCourseBaselineNativeInstaller& installer, std::span<uint8_t> parent, std::span<uint8_t> full)
        : native(installer), parent(parent), full(full) {}
    bool prepare(const char* destination, const char* candidate, const ContentManifest& manifest,
                 const TransferState& state, std::span<uint8_t> scratch) override {
      return matches(scratch) && native.prepare(destination, candidate, manifest, state, full);
    }
    bool metadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                  std::span<uint8_t> scratch) override {
      return matches(scratch) && native.metadata(destination, manifest, state, full);
    }

   private:
    HalCourseBaselineNativeInstaller& native;
    std::span<uint8_t> parent, full;
    bool matches(std::span<uint8_t> scratch) const {
      if (scratch.data() == parent.data() && scratch.size() == parent.size()) return true;
      LOG_ERR("COMPANION", "Baseline parent workspace mismatch");
      return false;
    }
  };
  // Reverse destruction releases the callback user before its target/parser.
  tinta::core::pack::Pack parser;
  HalCourseBaselineLearnerInspection inspection;
  HalCourseBaselineNativeInstaller nativeInstaller;
  ParentInstaller parentInstaller;
};

inline std::unique_ptr<HalCourseBaselineImportSession> createHalCourseBaselineImportSession(
    const Identity& reader, const Identity& generation, const Identity& owner, std::span<uint8_t> scratch,
    CourseBaselineSessionLimits limits, HalCourseBaselineLearnerInspection::Permission permitted, void* context,
    std::span<uint8_t> parentWorkspace = {}) {
  if (parentWorkspace.empty()) parentWorkspace = scratch;
  const auto fullStart = reinterpret_cast<uintptr_t>(scratch.data());
  const auto parentStart = reinterpret_cast<uintptr_t>(parentWorkspace.data());
  constexpr size_t PEAK_BYTES =
      sizeof(HalCourseBaselineImportSession) +
      std::max(sizeof(HalHistoricalCourseHistory), sizeof(HalCourseBaselineReviewedJournalAudit));
  if (reader == Identity{} || generation == Identity{} || owner == Identity{} || !permitted || !permitted(context) ||
      !limits.depth || !limits.screens || !limits.queued || scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
      parentWorkspace.size() < TRANSFER_JOURNAL_SIZE || parentWorkspace.size() > scratch.size() ||
      parentStart < fullStart || parentStart - fullStart > scratch.size() - parentWorkspace.size() ||
      !admitCompanionHeap(PEAK_BYTES, sizeof(HalCourseBaselineImportSession))) {
    LOG_ERR("COMPANION", "Baseline import session admission failed");
    return nullptr;
  }
  auto session = makeUniqueNoThrow<HalCourseBaselineImportSession>(reader, generation, owner, scratch, limits,
                                                                   permitted, context, parentWorkspace);
  if (!session) LOG_ERR("COMPANION", "OOM: baseline import session");
  return session;
}
}  // namespace companion

#endif
