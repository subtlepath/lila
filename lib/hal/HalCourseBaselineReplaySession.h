#pragma once

#if LILA_TINTA

#include "HalCourseBaselineReviewedJournalAudit.h"
#include "HalTintaReplayStore.h"

namespace companion {
// Admit off stack. Caller freezes reviewed backups, catalog and replay writers.
// This projects journal events into disposable files; legacy cache equivalence
// and saved-session receipt binding require separate proof.
class HalCourseBaselineReplaySession final {
 public:
  using Permission = HalCourseBaselineReviewedJournalAudit::Permission;
  HalCourseBaselineReplaySession(const tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source,
                                 Permission permitted, void* context)
      : pack(pack), source(source), permitted(permitted), context(context) {}
  bool run(std::span<const uint8_t> review, const Digest& expected, const Identity& course,
           std::span<uint8_t> scratch) {
    ready = false;
    if (course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &pack, sizeof(pack)) ||
        course_baseline_detail::overlaps(review.data(), review.size(), this, sizeof(*this)) || !store.close() ||
        !guard())
      return failure("workspace, admission or close");
    auto audit = createHalCourseBaselineReviewedJournalAudit(pack, source, permitted, context);
    if (!audit || !audit->run(review, expected, course, scratch)) return failure("reviewed journal audit");
    // Keep reducer state outside the replay frame; one allocation per projection.
    constexpr size_t REPLAY_BYTES = sizeof(TintaReplayReducer) + HalJournalCausalAuditSession::replayWorkspaceBytes();
    constexpr size_t LARGEST_REPLAY =
        std::max(sizeof(TintaReplayReducer), HalJournalCausalAuditSession::replayWorkspaceBytes());
    if (!guard() || !admitCompanionHeap(REPLAY_BYTES, LARGEST_REPLAY) || !store.begin(course))
      return failure("working store");
    auto reducer = makeUniqueNoThrow<TintaReplayReducer>(store, course);
    if (!reducer) return failure("OOM: reducer");
    if (!audit->replay(reducer.get(), reduce, scratch, true) || !guard()) return failure("reviewed journal replay");
    const auto* verified = audit->journalFrontier();
    if (!verified || !store.matchesCourse(course) || !guard()) return failure("projection binding");
    frontier = *verified;
    ready = true;
    return true;
  }
  HalTintaReplayStore* workingStore() { return ready && guard() ? &store : nullptr; }
  const Digest* journalFrontier() const { return ready && guard() ? &frontier : nullptr; }

 private:
  const tinta::core::pack::Pack& pack;
  tinta::core::pack::PackSource& source;
  Permission permitted;
  void* context;
  HalTintaReplayStore store{TintaReplayStoreTarget::BaselineProof};
  Digest frontier{};
  bool ready = false;
  bool guard() const { return permitted && permitted(context) && Storage.ready() && admitCompanionHeap(); }
  bool failure(const char* stage) {
    ready = false;
    LOG_ERR("COMPANION", "Baseline replay projection refused: %s", stage);
    store.close();
    return false;
  }
  static bool reduce(void* context, uint32_t, const SyncEvent& event, std::span<const uint8_t> bytes, bool undone) {
    const auto result = static_cast<TintaReplayReducer*>(context)->apply(event, bytes, undone);
    if (result == TintaJournalResult::Ok) return true;
    LOG_ERR("COMPANION", "Baseline replay reduction refused: %u", static_cast<unsigned>(result));
    return false;
  }
};
inline std::unique_ptr<HalCourseBaselineReplaySession> createHalCourseBaselineReplaySession(
    const tinta::core::pack::Pack& pack, tinta::core::pack::PackSource& source,
    HalCourseBaselineReplaySession::Permission permitted, void* context) {
  if (!permitted || !permitted(context) || !pack.isOpen() ||
      !admitCompanionHeap(sizeof(HalCourseBaselineReplaySession), sizeof(HalCourseBaselineReplaySession))) {
    LOG_ERR("COMPANION", "Baseline replay session admission refused");
    return nullptr;
  }
  auto session = makeUniqueNoThrow<HalCourseBaselineReplaySession>(pack, source, permitted, context);
  if (!session) LOG_ERR("COMPANION", "OOM: baseline replay session");
  return session;
}
}  // namespace companion
#endif
