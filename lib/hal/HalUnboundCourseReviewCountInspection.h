#pragma once

#if LILA_TINTA
#include "HalUnboundCourseReviewReader.h"

namespace companion {
struct UnboundCourseReviewCounts {
  uint32_t records = 0, events = 0;
};
// Retain off stack with exclusive stream/workspace owners and excluded writers.
// Counts are frozen-stream evidence; epoch ownership and replay remain separate.
class HalUnboundCourseReviewCountInspection final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseReviewCountInspection(HalUnboundCourseMigrationInspection& inspection,
                                        HalUnboundCourseReviewReader& stream, std::span<uint8_t> scratch,
                                        Permission permitted, void* context)
      : inspection(inspection), stream(stream), scratch(scratch), permitted(permitted), context(context) {}
  ~HalUnboundCourseReviewCountInspection() { closeReaders(); }
  HalUnboundCourseReviewCountInspection(const HalUnboundCourseReviewCountInspection&) = delete;
  HalUnboundCourseReviewCountInspection& operator=(const HalUnboundCourseReviewCountInspection&) = delete;
  bool inspect(const UnboundCourseMigrationIntent& input) {
    if (operating) return false;
    ready = false;
    if (!validUnboundCourseMigrationIntent(input) || scratch.size() < SESSION_WORKSPACE_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return failure();
    selected = input;
    result = {};
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && guard() && inspection.report(selected) && stream.open(selected) && count();
    if (valid) {
      const auto* evidence = inspection.report(selected);
      valid = evidence && evidence->learner.reviews.journal.records == result.records && guard();
    }
    const bool closed = releaseReaders();
    ready = valid && closed && guard() && inspection.report(selected) && !cancelled;
    operating = false;
    return ready || failure();
  }
  const UnboundCourseReviewCounts* report(const UnboundCourseMigrationIntent& input) const {
    if (operating || !ready || input != selected) return nullptr;
    operating = true;
    if (!guard() || !inspection.report(selected) || cancelled) ready = false;
    operating = false;
    return ready && input == selected ? &result : nullptr;
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseReviewReader& stream;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseMigrationIntent selected;
  UnboundCourseReviewCounts result;
  mutable bool operating = false, ready = false;
  bool cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  bool releaseReaders() {
    ready = false;
    return stream.closeReaders();
  }
  [[gnu::noinline]] bool count() {
    for (;;) {
      UnboundCourseReviewEntry entry;
      if (!guard()) return false;
      const auto read = stream.next(selected, entry);
      if (read == LegacyTintaReadResult::End) return guard();
      if (read != LegacyTintaReadResult::Record || entry.index != result.records || !guard()) return false;
      uint32_t events = 1;
      switch (entry.entry.operation) {
        case LegacyTintaOperation::Review:
        case LegacyTintaOperation::Undo:
          break;
        case LegacyTintaOperation::Flags:
          events = 2;
          break;
        default:
          return false;
      }
      if (result.records == UINT32_MAX || result.events > UINT32_MAX - events) return false;
      ++result.records;
      result.events += events;
      if (result.records % 32 == 0) vTaskDelay(1);
    }
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course review count inspection refused");
    return false;
  }
};
}  // namespace companion
#endif
