#pragma once

#if LILA_TINTA

#include "HalCourseBaselineReplaySession.h"
#include "HalTintaReplayExport.h"

namespace companion {
// Admit off stack. Owned receipt bytes outlive exports that reuse the scratch
// loan. Caller verifies the original pack and immutable reviewed backup cohort.
class HalCourseBaselineReplayReceipt final {
 public:
  using Permission = HalCourseBaselineReplaySession::Permission;
  HalCourseBaselineReplayReceipt(Permission permitted, void* context) : permitted(permitted), context(context) {}
  bool run(HalCourseBaselineReplaySession& replay, std::span<const uint8_t> input, const Digest& expected,
           const Identity& course, const Identity& generation, const Digest& pack, std::span<uint8_t> scratch) {
    ready = false;
    if (!guard() || input.size() != receipt.size() || scratch.size() < 80 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &replay, sizeof(replay)))
      return failure("arguments or workspace");
    std::copy(input.begin(), input.end(), receipt.begin());
    const auto* frontier = replay.journalFrontier();
    auto* store = replay.workingStore();
    TintaDerivedManifestView manifest;
    if (!frontier || !store || !manifest.decode(receipt) || !manifest.matches(course, generation, pack, *frontier) ||
        mbedtls_sha256(receipt.data(), receipt.size(), digest.data(), 0) != 0 || digest != expected || !guard())
      return failure("receipt hash or binding");
    if (!output.run(*store, course, manifest.studyDay(), scratch)) return failure("replay export");
    for (unsigned at = 0; at < 5; ++at) {
      uint64_t length = 0;
      Digest hash{};
      const auto kind = static_cast<TintaDerivedFile>(at);
      if (!guard() || !output.receipt(kind, length, hash) || length != manifest.length(kind) ||
          !std::equal(hash.begin(), hash.end(), manifest.hash(kind).begin()))
        return failure("derived file correspondence");
    }
    ready = replay.journalFrontier() && replay.workingStore() && guard();
    return ready || failure("final permission");
  }
  const Digest* sessionSnapshot() const { return ready && guard() ? &digest : nullptr; }

 private:
  Permission permitted;
  void* context;
  HalTintaReplayExport output{TintaReplayExportTarget::BaselineProof};
  std::array<uint8_t, TINTA_DERIVED_MANIFEST_SIZE> receipt{};
  Digest digest{};
  bool ready = false;
  bool guard() const { return permitted && permitted(context) && Storage.ready() && admitCompanionHeap(); }
  bool failure(const char* stage) {
    ready = false;
    LOG_ERR("COMPANION", "Baseline replay receipt refused: %s", stage);
    return false;
  }
};
inline std::unique_ptr<HalCourseBaselineReplayReceipt> createHalCourseBaselineReplayReceipt(
    HalCourseBaselineReplayReceipt::Permission permitted, void* context) {
  if (!permitted || !permitted(context) ||
      !admitCompanionHeap(sizeof(HalCourseBaselineReplayReceipt), sizeof(HalCourseBaselineReplayReceipt))) {
    LOG_ERR("COMPANION", "Baseline replay receipt admission refused");
    return nullptr;
  }
  auto proof = makeUniqueNoThrow<HalCourseBaselineReplayReceipt>(permitted, context);
  if (!proof) LOG_ERR("COMPANION", "OOM: baseline replay receipt");
  return proof;
}
}  // namespace companion
#endif
