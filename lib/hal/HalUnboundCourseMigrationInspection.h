#pragma once

#if LILA_TINTA
#include "CompanionFrame.h"
#include "HalUnboundCourseBoundReadingMapping.h"
#include "HalUnboundCoursePackVerification.h"

namespace companion {
struct UnboundCourseMigrationReport {
  UnboundCourseLearnerReport learner;
  TintaLegacyLessonMapping lessons;
  UnboundCourseReadingMappingReport readings;
};
// Admit off stack. The native parent lends exclusive owners/parser/workspace,
// excludes writers, and supplies its authenticated reader/generation/owner.
// This is read-only evidence; provenance, replay and publication remain separate.
class HalUnboundCourseMigrationInspection final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseMigrationInspection(const Identity& reader, const Identity& generation, const Identity& owner,
                                      HalUnboundCoursePackVerification& pair, HalUnboundCoursePackReader& original,
                                      HalUnboundCoursePackReader& installed, HalUnboundCourseLearnerInspection& learner,
                                      HalUnboundCourseReviewedFile& reviewed, std::span<uint8_t> scratch,
                                      Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        pair(pair),
        original(original),
        installed(installed),
        learner(learner),
        reviewed(reviewed),
        scratch(scratch),
        permitted(permitted),
        context(context) {}
  ~HalUnboundCourseMigrationInspection() { closeReaders(); }
  HalUnboundCourseMigrationInspection(const HalUnboundCourseMigrationInspection&) = delete;
  HalUnboundCourseMigrationInspection& operator=(const HalUnboundCourseMigrationInspection&) = delete;
  bool inspect(const UnboundCourseMigrationIntent& input, std::string_view originalPath) {
    if (operating) return false;
    ready = false;
    if (!validUnboundCourseMigrationIntent(input) || reader == Identity{} || generation == Identity{} ||
        owner == Identity{} || input.reader != reader || input.request.original.generation != generation ||
        input.request.original.owner != owner || &original == &installed || scratch.size() < SESSION_WORKSPACE_SIZE ||
        originalPath.empty() || originalPath.size() >= path.size() ||
        originalPath.find('\0') != std::string_view::npos ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(originalPath.data(), originalPath.size(), this, sizeof(*this)))
      return failure();
    selected = input;
    std::copy(originalPath.begin(), originalPath.end(), path.begin());
    path[originalPath.size()] = 0;
    resetResult();
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && guard() && pair.verify(selected, path.data()) && pair.verified(selected) &&
                 original.open(selected, UnboundPackRole::Original, path.data()) &&
                 installed.open(selected, UnboundPackRole::Installed) && inspectLearner() && mapLessons() &&
                 mapReadings();
    if (valid) {
      const auto* evidence = learner.report(selected.request);
      valid = evidence && guard();
      if (valid) result.learner = *evidence;
    }
    if (valid)
      valid = original.recheck(selected, UnboundPackRole::Original) &&
              installed.recheck(selected, UnboundPackRole::Installed) && guard();
    const bool closed = releaseReaders();
    ready = valid && closed && guard() && !cancelled;
    operating = false;
    return ready || failure();
  }
  const UnboundCourseMigrationReport* report(const UnboundCourseMigrationIntent& input) const {
    if (operating || !ready || input != selected) return nullptr;
    operating = true;
    if (!guard()) ready = false;
    operating = false;
    return ready && input == selected ? &result : nullptr;
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    return releaseReaders();
  }

 private:
  Identity reader, generation, owner;
  HalUnboundCoursePackVerification& pair;
  HalUnboundCoursePackReader& original;
  HalUnboundCoursePackReader& installed;
  HalUnboundCourseLearnerInspection& learner;
  HalUnboundCourseReviewedFile& reviewed;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntent selected;
  UnboundCourseMigrationReport result;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  mutable bool operating = false, ready = false;
  bool cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseMigrationInspection*>(context)->guard(); }
  bool releaseReaders() {
    ready = false;
    const bool a = learner.closeReaders();
    const bool b = reviewed.closeReaders();
    const bool c = installed.closeReaders();
    const bool d = original.closeReaders();
    const bool e = pair.closeReaders();
    return a && b && c && d && e;
  }
  [[gnu::noinline]] void resetResult() { result = {}; }
  [[gnu::noinline]] bool inspectLearner() {
    const auto loan = original.borrowed(selected, UnboundPackRole::Original);
    const auto installedLoan = installed.borrowed(selected, UnboundPackRole::Installed);
    return loan && installedLoan && loan.pack != installedLoan.pack && loan.source != installedLoan.source && guard() &&
           learner.inspect(selected.request, *loan.source, *loan.pack) && guard();
  }
  [[gnu::noinline]] bool mapLessons() {
    return mapUnboundCourseBoundLessons(learner, original, installed, selected, scratch, result.lessons, allowed, this);
  }
  [[gnu::noinline]] bool mapReadings() {
    return mapUnboundCourseBoundReadings(learner, reviewed, original, installed, selected, scratch, result.readings,
                                         allowed, this);
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course migration inspection refused");
    return false;
  }
};
}  // namespace companion
#endif
