#pragma once

#include "HalCourseBaselineReviewBackup.h"
#include "HalUnboundCourseDayInspection.h"
#include "HalUnboundCourseMarkInspection.h"
#include "HalUnboundCourseProfileInspection.h"
#include "HalUnboundCourseSessionInspection.h"

namespace companion {
struct UnboundCourseLearnerReport {
  UnboundCourseItemReport items;
  UnboundCourseReviewReport reviews;
  UnboundCourseProfileReport profile;
  UnboundCourseMarkReport stars, readings;
  UnboundCourseDayReport days;
  UnboundCourseSessionReport session;
};
// Admit off stack. Caller validates the immutable original pack/source, excludes
// writers and lends exclusive reviewed-file/backup owners with the same workspace.
class HalUnboundCourseLearnerInspection final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseLearnerInspection(const Identity& reader, const Identity& generation,
                                    HalUnboundCourseReviewedFile& reviewed, HalCourseBaselineReviewBackup& backups,
                                    std::span<uint8_t> scratch, Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        reviewed(reviewed),
        backups(backups),
        scratch(scratch),
        permitted(permitted),
        context(context) {}
  ~HalUnboundCourseLearnerInspection() { closeReaders(); }
  HalUnboundCourseLearnerInspection(const HalUnboundCourseLearnerInspection&) = delete;
  HalUnboundCourseLearnerInspection& operator=(const HalUnboundCourseLearnerInspection&) = delete;
  bool inspect(const UnboundCourseMigrationRequest& request, tinta::core::pack::PackSource& source,
               const tinta::core::pack::Pack& pack) {
    if (operating) return false;
    ready = false;
    if (reader == Identity{} || generation == Identity{} || request.original.generation != generation ||
        !validCourseBaselineImportRequest(request.original) || !pack.isOpen() ||
        scratch.size() < unbound_course_detail::DAY_COVERAGE_BYTES ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &request, sizeof(request)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &pack, sizeof(pack)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &source, sizeof(source)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &reviewed, sizeof(reviewed)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &backups, sizeof(backups)))
      return failure();
    selected = request;
    result = {};
    operating = true;
    const bool valid = guard() && closeReaders() && verifyBackups() && inspectItems(source) && inspectReviews(source) &&
                       inspectProfile(pack) && inspectMarks(source, pack, false) && inspectMarks(source, pack, true) &&
                       inspectDays() && inspectSession(source, pack) && verifyBackups();
    const bool closed = closeReaders();
    ready = valid && closed && guard();
    operating = false;
    return ready || failure();
  }
  const UnboundCourseLearnerReport* report() const {
    if (operating) return nullptr;
    if (!guard()) ready = false;
    return ready ? &result : nullptr;
  }
  bool closeReaders() {
    ready = false;
    const bool reviewedClosed = reviewed.closeReaders();
    const bool backupsClosed = backups.closeReaders();
    return reviewedClosed && backupsClosed;
  }

 private:
  Identity reader, generation;
  HalUnboundCourseReviewedFile& reviewed;
  HalCourseBaselineReviewBackup& backups;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationRequest selected;
  UnboundCourseLearnerReport result;
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseLearnerInspection*>(context)->guard(); }
  bool failure() {
    ready = false;
    LOG_ERR("COMPANION", "Unbound learner cohort inspection refused");
    return false;
  }
  [[gnu::noinline]] bool verifyBackups() {
    return guard() && backups.verifyStoredUnbound(selected.original.reviewHash, reader, generation,
                                                  selected.original.manifest.logicalIdentity);
  }
  [[gnu::noinline]] bool inspectItems(tinta::core::pack::PackSource& source) {
    return inspectUnboundCourseItems(reviewed, selected, source, scratch, result.items, allowed, this);
  }
  [[gnu::noinline]] bool inspectReviews(tinta::core::pack::PackSource& source) {
    return inspectUnboundCourseReviews(reviewed, selected, source, result.items, scratch, result.reviews, allowed,
                                       this);
  }
  [[gnu::noinline]] bool inspectProfile(const tinta::core::pack::Pack& pack) {
    return inspectUnboundCourseProfile(reviewed, selected, pack, scratch, result.profile, allowed, this);
  }
  [[gnu::noinline]] bool inspectMarks(tinta::core::pack::PackSource& source, const tinta::core::pack::Pack& pack,
                                      bool readings) {
    return inspectUnboundCourseMarks(reviewed, selected, source, pack,
                                     readings ? UnboundCourseMarkKind::Readings : UnboundCourseMarkKind::Stars, scratch,
                                     readings ? result.readings : result.stars, allowed, this);
  }
  [[gnu::noinline]] bool inspectDays() {
    return inspectUnboundCourseDays(reviewed, selected, scratch, result.days, allowed, this);
  }
  [[gnu::noinline]] bool inspectSession(tinta::core::pack::PackSource& source, const tinta::core::pack::Pack& pack) {
    return inspectUnboundCourseSession(reviewed, selected, source, pack, result.items, result.reviews, scratch,
                                       result.session, allowed, this);
  }
};
}  // namespace companion
