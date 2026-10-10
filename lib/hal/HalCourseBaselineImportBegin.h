#pragma once

#if LILA_TINTA

#include "CompanionCourseBaselineTransfer.h"
#include "CompanionWorkspace.h"
#include "HalCourseBaselineImportConsentStore.h"
#include "HalCourseBaselineJournalReadiness.h"

namespace companion {
// Caller authenticates owner, closes activities and leases the full workspace.
// A failed begin may leave durable consent for orphan-consent recovery.
inline TransferResult beginHalCourseBaselineImport(Transfer& transfer, const Identity& reader,
                                                   const Identity& generation, const Identity& owner,
                                                   const CourseBaselineImportRequest& request,
                                                   std::span<uint8_t> scratch,
                                                   HalCourseBaselineImportConsentStore::Permission permitted,
                                                   void* context) {
  if (!permitted || !permitted(context)) return TransferResult::Unauthorized;
  if (reader == Identity{} || scratch.size() != SESSION_WORKSPACE_SIZE ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &request, sizeof(request)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), &transfer, sizeof(transfer)) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), reader.data(), reader.size()) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), generation.data(), generation.size()) ||
      course_baseline_detail::overlaps(scratch.data(), scratch.size(), owner.data(), owner.size())) {
    LOG_ERR("COMPANION", "Baseline begin workspace/identity loan refused");
    return TransferResult::Invalid;
  }
  struct Approval {
    const Identity& reader;
    const Identity& generation;
    const Identity& owner;
    std::span<uint8_t> scratch;
    HalCourseBaselineImportConsentStore::Permission permitted;
    void* context;
  } approval{reader, generation, owner, scratch, permitted, context};
  return beginCourseBaselineTransfer(
      transfer, request, generation, owner,
      [](void* context, const CourseBaselineImportRequest& selected, const TransferDeclaration& declaration) {
        auto& approval = *static_cast<Approval*>(context);
        if (!prepareHalCourseBaselineJournal(approval.permitted, approval.context)) return false;
        constexpr size_t PEAK_BYTES = sizeof(HalCourseBaselineImportConsentStore) + sizeof(HalHistoricalCourseHistory) +
                                      sizeof(HalCoursePackHistory) + sizeof(HalCourseBaselineReviewBackup);
        constexpr size_t LARGEST_BYTES =
            std::max({sizeof(HalCourseBaselineImportConsentStore), sizeof(HalHistoricalCourseHistory),
                      sizeof(HalCoursePackHistory), sizeof(HalCourseBaselineReviewBackup)});
        if (!approval.permitted(approval.context) || !admitCompanionHeap(PEAK_BYTES, LARGEST_BYTES)) return false;
        // Paths, metadata handles and retained request exceed the local budget.
        auto consent = makeUniqueNoThrow<HalCourseBaselineImportConsentStore>(approval.reader, approval.generation,
                                                                              approval.owner, approval.scratch,
                                                                              approval.permitted, approval.context);
        if (!consent) {
          LOG_ERR("COMPANION", "OOM: baseline begin consent store");
          return false;
        }
        const auto result = consent->approve(selected, declaration);
        const bool closed = consent->close();
        return result == CourseBaselineConsentResult::Ok && closed && approval.permitted(approval.context);
      },
      &approval);
}
}  // namespace companion

#endif
