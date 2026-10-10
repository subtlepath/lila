#pragma once

#if LILA_TINTA
#include "HalLegacyTintaJournalReader.h"
#include "HalUnboundCourseMigrationInspection.h"

namespace companion {
struct UnboundCourseReviewEntry {
  LegacyTintaEntry entry;
  uint32_t index = 0;
  bool committed = false;
};
// Retain off stack with exclusive reviewed-file/workspace owners and no writers.
// Records are provisional conversion input until End; no distributed identity
// or trustworthy clock is inferred from a legacy timestamp or record index.
class HalUnboundCourseReviewReader final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseReviewReader(HalUnboundCourseMigrationInspection& inspection, HalUnboundCourseReviewedFile& reviewed,
                               std::span<uint8_t> scratch, Permission permitted, void* context)
      : inspection(inspection), reviewed(reviewed), scratch(scratch), permitted(permitted), context(context) {}
  ~HalUnboundCourseReviewReader() { closeReaders(); }
  HalUnboundCourseReviewReader(const HalUnboundCourseReviewReader&) = delete;
  HalUnboundCourseReviewReader& operator=(const HalUnboundCourseReviewReader&) = delete;
  bool open(const UnboundCourseMigrationIntent& input) {
    if (operating) return false;
    ready = complete = false;
    if (scratch.size() < SESSION_WORKSPACE_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return failure();
    selected = input;
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && guard();
    const auto* evidence = valid ? inspection.report(selected) : nullptr;
    valid = evidence != nullptr;
    if (valid) {
      present = evidence->learner.reviews.present;
      expected = evidence->learner.reviews.journal.records;
      committed = evidence->learner.items.committedRecords;
      valid = committed <= expected && (present || !expected);
    }
    if (valid) {
      const auto opened = reviewed.open(selected.request, "reviews.log");
      valid = present ? opened == UnboundReviewedFileResult::Present : opened == UnboundReviewedFileResult::Missing;
      if (valid && present) {
        auto* file = reviewed.borrowed();
        valid = file && reader.attach(*file, scratch, allowed, this);
      }
    }
    index = 0;
    ready = valid && guard() && inspection.report(selected) && !cancelled;
    if (!ready) releaseReaders();
    operating = false;
    return ready || failure();
  }
  LegacyTintaReadResult next(const UnboundCourseMigrationIntent& input, UnboundCourseReviewEntry& output) {
    if (operating || !ready || input != selected ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &output, sizeof(output)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &output, sizeof(output)))
      return LegacyTintaReadResult::Unavailable;
    operating = true;
    if (!guard() || !inspection.report(selected)) return finish(LegacyTintaReadResult::Unavailable, input);
    if (complete) return finish(LegacyTintaReadResult::End, input);
    UnboundCourseReviewEntry record;
    const auto result = present ? reader.next(record.entry) : LegacyTintaReadResult::End;
    if (result == LegacyTintaReadResult::Record) {
      if (index >= expected || !guard() || !inspection.report(selected) || input != selected)
        return finish(LegacyTintaReadResult::Unavailable, input);
      record.index = index;
      record.committed = index < committed;
      ++index;
      const auto finished = finish(LegacyTintaReadResult::Record, input);
      if (finished == LegacyTintaReadResult::Record) output = record;
      return finished;
    }
    if (result != LegacyTintaReadResult::End) return finish(result, input);
    // Reopening hashes the frozen roster-bound copy after all records were read.
    if (index != expected || !releaseReaders() || !guard()) return finish(LegacyTintaReadResult::IoError, input);
    const auto opened = reviewed.open(selected.request, "reviews.log");
    const bool valid =
        (present ? opened == UnboundReviewedFileResult::Present : opened == UnboundReviewedFileResult::Missing) &&
        reviewed.closeReaders() && guard() && inspection.report(selected) && input == selected;
    ready = complete = valid;
    return finish(valid ? LegacyTintaReadResult::End : LegacyTintaReadResult::IoError, input);
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseReviewedFile& reviewed;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntent selected;
  HalLegacyTintaJournalReader reader;
  uint32_t expected = 0, committed = 0, index = 0;
  bool operating = false, ready = false, complete = false, present = false, cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseReviewReader*>(context)->guard(); }
  bool releaseReaders() {
    ready = complete = false;
    reader.detach();
    return reviewed.closeReaders();
  }
  LegacyTintaReadResult finish(LegacyTintaReadResult result, const UnboundCourseMigrationIntent& input) {
    if (!guard() || !inspection.report(selected) || cancelled || input != selected)
      result = LegacyTintaReadResult::Unavailable;
    if (result != LegacyTintaReadResult::Record && result != LegacyTintaReadResult::End) {
      releaseReaders();
      LOG_ERR("COMPANION", "Unbound course review stream refused");
    }
    operating = false;
    return result;
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course review reader refused");
    return false;
  }
};
}  // namespace companion
#endif
