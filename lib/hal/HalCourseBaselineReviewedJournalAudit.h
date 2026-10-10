#pragma once

#if LILA_TINTA

#include "CompanionCourseBaselineJournalSnapshot.h"
#include "CompanionTintaPackSubjectCatalog.h"
#include "HalCompanionHeapAdmission.h"
#include "HalJournalCausalAuditSession.h"
#include "HalTransferStorage.h"

namespace companion {
// Temporary checked owner: journal record/index buffers and paths exceed the
// small task stack. Borrowed pack/source and permission context outlive it.
class HalCourseBaselineReviewedJournalAudit final {
 public:
  using Permission = CourseBaselineJournalSnapshot::Permission;
  HalCourseBaselineReviewedJournalAudit(const tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source,
                                        Permission permitted, void* context)
      : permitted(permitted),
        context(context),
        snapshot(storage, allowed, this, hash, this),
        subjects(pack, source),
        audit(snapshot, close, this) {}
  bool run(std::span<const uint8_t> review, const Digest& expected, const Identity& course,
           std::span<uint8_t> scratch) {
    ready = false;
    if (!guard() || review.size() < 56 || !std::equal(course.begin(), course.end(), review.begin() + 40) ||
        snapshot.open(review, expected, scratch) != CourseBaselineJournalSnapshotResult::Ok ||
        !subjects.prepare(scratch) || !guard() || !audit.run(nullptr, &course, &subjects) || !guard()) {
      LOG_ERR("COMPANION", "Reviewed baseline journal audit refused");
      return false;
    }
    ready = true;
    return true;
  }
  // Callback consumes borrowed records immediately; scratch may be reused.
  bool replay(void* replayContext, HalJournalCausalAuditSession::ReplayVisitor visitor, std::span<uint8_t> scratch,
              bool markUndoneReviews = true) {
    const bool admitted = ready && visitor && guard() &&
                          admitCompanionHeap(HalJournalCausalAuditSession::replayWorkspaceBytes(),
                                             HalJournalCausalAuditSession::replayWorkspaceBytes());
    ready = false;
    if (!admitted || snapshot.reopen(scratch) != CourseBaselineJournalSnapshotResult::Ok) {
      snapshot.close();
      LOG_ERR("COMPANION", "Reviewed baseline replay admission or source refused");
      return false;
    }
    const bool replayed = audit.replay(replayContext, visitor, markUndoneReviews);
    const bool verified = replayed && guard() && snapshot.reopen(scratch) == CourseBaselineJournalSnapshotResult::Ok;
    snapshot.close();
    if (!verified || !guard()) {
      LOG_ERR("COMPANION", "Reviewed baseline replay or final source proof refused");
      return false;
    }
    return true;
  }

 private:
  Permission permitted;
  void* context;
  HalTransferStorage storage;
  CourseBaselineJournalSnapshot snapshot;
  TintaPackSubjectCatalog subjects;
  HalJournalCausalAuditSession audit;
  bool ready = false;
  bool guard() const { return permitted && permitted(context) && Storage.ready() && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineReviewedJournalAudit*>(context)->guard(); }
  static bool hash(void* context, std::span<const uint8_t> bytes, Digest& output) {
    auto& owner = *static_cast<HalCourseBaselineReviewedJournalAudit*>(context);
    return owner.guard() && mbedtls_sha256(bytes.data(), bytes.size(), output.data(), 0) == 0 && owner.guard();
  }
  static bool close(void* context) {
    static_cast<HalCourseBaselineReviewedJournalAudit*>(context)->snapshot.close();
    return true;
  }
};
inline std::unique_ptr<HalCourseBaselineReviewedJournalAudit> createHalCourseBaselineReviewedJournalAudit(
    const tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source,
    HalCourseBaselineReviewedJournalAudit::Permission permitted, void* context) {
  if (!permitted || !permitted(context) || !pack.isOpen() ||
      !admitCompanionHeap(sizeof(HalCourseBaselineReviewedJournalAudit),
                          sizeof(HalCourseBaselineReviewedJournalAudit))) {
    LOG_ERR("COMPANION", "Reviewed baseline journal audit admission refused");
    return nullptr;
  }
  auto owner = makeUniqueNoThrow<HalCourseBaselineReviewedJournalAudit>(pack, source, permitted, context);
  if (!owner) LOG_ERR("COMPANION", "OOM: reviewed baseline journal audit");
  return owner;
}
}  // namespace companion
#endif
