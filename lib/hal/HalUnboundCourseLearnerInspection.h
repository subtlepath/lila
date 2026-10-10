#pragma once

#include <mbedtls/sha256.h>

#include "CompanionCourseValidation.h"
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
// Admit off stack. Caller confirms the original payload, excludes writers and
// lends exclusive parser/reviewed-file/backup owners with the same workspace.
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
               tinta::core::pack::Pack& pack) {
    if (operating) return false;
    ready = false;
    if (reader == Identity{} || generation == Identity{} || request.original.generation != generation ||
        !validCourseBaselineImportRequest(request.original) ||
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
    const bool valid = guard() && closeReaders() && verifySource(source) && validateSource(source, pack) &&
                       verifyBackups() && inspectItems(source) && inspectReviews(source) && inspectProfile(pack) &&
                       inspectMarks(source, pack, false) && inspectMarks(source, pack, true) && inspectDays() &&
                       inspectSession(source, pack) && verifyBackups() && verifySource(source);
    const bool closed = closeReaders();
    ready = valid && closed && guard();
    operating = false;
    return ready || failure();
  }
  const UnboundCourseLearnerReport* report(const UnboundCourseMigrationRequest& request) const {
    if (operating || selected != request) return nullptr;
    operating = true;
    if (!guard()) ready = false;
    operating = false;
    return ready && selected == request ? &result : nullptr;
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
  mbedtls_sha256_context digest;
  mutable bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseLearnerInspection*>(context)->guard(); }
  bool failure() {
    ready = false;
    LOG_ERR("COMPANION", "Unbound learner cohort inspection refused");
    return false;
  }
  [[gnu::noinline]] bool verifySource(tinta::core::pack::PackSource& source) {
    const auto length = source.size();
    if (!guard() || length != selected.original.manifest.length) return false;
    mbedtls_sha256_init(&digest);
    int status = mbedtls_sha256_starts(&digest, 0);
    for (uint32_t offset = 0; status == 0 && offset < length;) {
      const auto count = static_cast<uint32_t>(std::min<size_t>(scratch.size(), length - offset));
      if (!guard() || !source.read(offset, scratch.data(), count)) {
        status = -1;
        break;
      }
      status = mbedtls_sha256_update(&digest, scratch.data(), count);
      offset += count;
      vTaskDelay(1);
    }
    Digest actual{};
    if (status == 0) status = mbedtls_sha256_finish(&digest, actual.data());
    mbedtls_sha256_free(&digest);
    return status == 0 && source.size() == length && actual == selected.original.manifest.contentHash && guard();
  }
  [[gnu::noinline]] bool validateSource(tinta::core::pack::PackSource& source, tinta::core::pack::Pack& pack) {
    return guard() && validateCourseCandidate(pack, source, scratch) == CourseValidationResult::Ok &&
           pack.formatMajor() == selected.original.manifest.formatVersion && guard();
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
