#pragma once

#if LILA_TINTA
#include <optional>

#include "HalUnboundCourseMigrationInspection.h"

namespace companion {
enum class UnboundCourseStarReadResult { Record, End, Unavailable, IoError };
struct UnboundCourseStarEntry {
  uint32_t uid = 0;
  uint16_t index = 0;
};
// Parent lends exclusive frozen-file/workspace owners and excludes writers.
// Retained UIDs include original-course retired items. Entries are provisional
// until End and carry no inferred event identity, timestamp or shared ancestry.
class HalUnboundCourseStarReader final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseStarReader(HalUnboundCourseMigrationInspection& inspection, HalUnboundCourseReviewedFile& reviewed,
                             std::span<uint8_t> scratch, Permission permitted, void* context)
      : inspection(inspection), reviewed(reviewed), scratch(scratch), permitted(permitted), context(context) {}
  ~HalUnboundCourseStarReader() { releaseReaders(); }
  HalUnboundCourseStarReader(const HalUnboundCourseStarReader&) = delete;
  HalUnboundCourseStarReader& operator=(const HalUnboundCourseStarReader&) = delete;
  bool open(const UnboundCourseMigrationIntent& input) {
    if (operating) return false;
    ready = complete = false;
    if (!validUnboundCourseMigrationIntent(input) || scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)))
      return failure();
    selected = input;
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && guard();
    const auto* evidence = valid ? inspection.report(selected) : nullptr;
    valid = evidence != nullptr;
    if (valid) {
      const auto& stars = evidence->learner.stars;
      present = stars.present;
      const uint32_t count = uint32_t(stars.catalog.matched) + stars.catalog.retired;
      valid = count <= tinta::core::library::MarkLog::kCapacity && (present || !count);
      expected = static_cast<uint16_t>(count);
    }
    if (valid) {
      const auto opened = reviewed.open(selected.request, "starred.bin");
      valid = present ? opened == UnboundReviewedFileResult::Present : opened == UnboundReviewedFileResult::Missing;
      if (valid && present) {
        auto* file = reviewed.borrowed();
        if (file) marks.emplace(*file);
        uint16_t count = 0;
        valid = file && marks->begin(scratch, allowed, this) && marks->entryCount(count) && count == expected;
      }
    }
    index = 0;
    ready = valid && scoped() && !cancelled && input == selected;
    if (!ready) releaseReaders();
    operating = false;
    return ready || failure();
  }
  UnboundCourseStarReadResult next(const UnboundCourseMigrationIntent& input, UnboundCourseStarEntry& output) {
    if (operating || !ready || input != selected ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &output, sizeof(output)) ||
        course_baseline_detail::overlaps(&input, sizeof(input), &output, sizeof(output)))
      return UnboundCourseStarReadResult::Unavailable;
    operating = true;
    if (!scoped()) return finish(UnboundCourseStarReadResult::Unavailable, input);
    if (complete) return finish(UnboundCourseStarReadResult::End, input);
    if (index < expected) {
      UnboundCourseStarEntry record;
      record.index = index;
      if (!marks || !marks->identityAt(index, record.uid) || !record.uid || record.uid == UINT32_MAX)
        return finish(UnboundCourseStarReadResult::IoError, input);
      ++index;
      const auto result = finish(UnboundCourseStarReadResult::Record, input);
      if (result == UnboundCourseStarReadResult::Record) output = record;
      return result;
    }
    if (!releaseReaders() || !guard()) return finish(UnboundCourseStarReadResult::IoError, input);
    const auto opened = reviewed.open(selected.request, "starred.bin");
    const bool valid =
        (present ? opened == UnboundReviewedFileResult::Present : opened == UnboundReviewedFileResult::Missing) &&
        reviewed.closeReaders() && scoped() && input == selected;
    ready = complete = valid;
    return finish(valid ? UnboundCourseStarReadResult::End : UnboundCourseStarReadResult::IoError, input);
  }
  bool closeReaders() {
    ready = complete = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseReviewedFile& reviewed;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntent selected;
  std::optional<HalTintaLegacyMarkView> marks;
  uint16_t expected = 0, index = 0;
  bool operating = false, ready = false, complete = false, present = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseStarReader*>(context)->guard(); }
  bool scoped() const {
    if (!guard()) return false;
    const auto* evidence = inspection.report(selected);
    return evidence && evidence->learner.stars.present == present &&
           uint32_t(evidence->learner.stars.catalog.matched) + evidence->learner.stars.catalog.retired == expected &&
           guard();
  }
  bool releaseReaders() {
    ready = complete = false;
    marks.reset();
    return reviewed.closeReaders();
  }
  UnboundCourseStarReadResult finish(UnboundCourseStarReadResult result, const UnboundCourseMigrationIntent& input) {
    if (!scoped() || cancelled || input != selected) result = UnboundCourseStarReadResult::Unavailable;
    if (result != UnboundCourseStarReadResult::Record && result != UnboundCourseStarReadResult::End) {
      releaseReaders();
      LOG_ERR("COMPANION", "Unbound course star stream refused");
    }
    operating = false;
    return result;
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course star reader refused");
    return false;
  }
};
}  // namespace companion
#endif
