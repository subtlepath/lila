#pragma once

#include <Memory.h>

#include "CompanionCourseBaselineImportRequest.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseBaselineReviewBackup.h"
#include "HalCoursePackHistory.h"
#include "HalHistoricalCourseHistory.h"

namespace companion {
// Caller supplies native/authenticated identities, establishes journal readiness
// and excludes all state/namespace writers through consent and upload admission.
// Preparation preserves evidence; it neither validates nor installs pack bytes.
class HalCourseBaselineImportPreparation final {
 public:
  using Permission = bool (*)(void*);
  using PersistApproval = bool (*)(void*, const CourseBaselineImportRequest&);
  HalCourseBaselineImportPreparation(const Identity& reader, const Identity& generation, const Identity& owner,
                                     std::span<uint8_t> scratch, Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        permitted(permitted),
        context(context) {}
  HalCourseBaselineImportPreparation(const HalCourseBaselineImportPreparation&) = delete;
  HalCourseBaselineImportPreparation& operator=(const HalCourseBaselineImportPreparation&) = delete;
  bool prepare(const CourseBaselineImportRequest& request, const TransferDeclaration& declaration,
               PersistApproval persist = nullptr, void* approvalContext = nullptr) {
    if (operating) return failure("reentry");
    ready = false;
    if (reader == Identity{} || scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        !matchesCourseBaselineImportRequest(request, generation, owner, request.reviewHash, declaration))
      return failure("request binding");
    selected = request;
    operating = true;
    const bool result = run(persist, approvalContext);
    operating = false;
    ready = result && guard();
    return ready || failure("preparation");
  }
  const CourseBaselineImportRequest* prepared() const {
    if (!guard()) ready = false;
    return ready ? &selected : nullptr;
  }
  void close() { ready = false; }

 private:
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  CourseBaselineImportRequest selected;
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineImportPreparation*>(context)->guard(); }
  bool run(PersistApproval persist, void* approvalContext) {
    if (!guard() || !admitCompanionHeap(sizeof(HalHistoricalCourseHistory), sizeof(HalHistoricalCourseHistory)))
      return failure("receipt heap admission");
    auto receipts = makeUniqueNoThrow<HalHistoricalCourseHistory>(generation, selected.manifest.logicalIdentity,
                                                                  scratch, allowed, this);
    if (!receipts) return failure("OOM: receipt history");
    if (receipts->inspect() != HistoricalCourseHistoryResult::Missing || !receipts->closeReaders())
      return failure("existing or unavailable receipt history");
    if (!guard() || !admitCompanionHeap(sizeof(HalCoursePackHistory), sizeof(HalCoursePackHistory)))
      return failure("archive heap admission");
    auto archives = makeUniqueNoThrow<HalCoursePackHistory>(scratch, allowed, this);
    if (!archives) return failure("OOM: archive history");
    if (!missing(*archives)) return false;
    if (!guard() || !admitCompanionHeap(sizeof(HalCourseBaselineReviewBackup), sizeof(HalCourseBaselineReviewBackup)))
      return failure("backup heap admission");
    auto backups = makeUniqueNoThrow<HalCourseBaselineReviewBackup>(scratch, allowed, this);
    if (!backups) return failure("OOM: reviewed backups");
    if (!backups->preserve(selected.reviewHash, reader, generation, selected.manifest.logicalIdentity) ||
        !backups->complete() || !backups->closeReaders())
      return failure("reviewed backups");
    if (persist && (!guard() || !persist(approvalContext, selected) || !guard()))
      return failure("approval persistence");
    // Reuse the admitted readers; scratch no longer carries a review loan.
    if (receipts->inspect() != HistoricalCourseHistoryResult::Missing || !receipts->closeReaders() ||
        !missing(*archives))
      return failure("history changed");
    if (persist && (!backups->preserve(selected.reviewHash, reader, generation, selected.manifest.logicalIdentity) ||
                    !backups->complete() || !backups->closeReaders()))
      return failure("state changed after approval");
    return guard();
  }
  bool missing(HalCoursePackHistory& archives) {
    const auto result = archives.visit(
        selected.manifest.logicalIdentity, [](void*, const ContentManifest&, const char*) { return true; }, nullptr);
    const bool closed = archives.closeReaders();
    return (result == CourseHistoryResult::MissingBaseline && closed && guard()) ||
           failure("existing, fresh or unavailable archive scope");
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Course baseline %s refused", operation);
    return false;
  }
};
}  // namespace companion
