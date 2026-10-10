#pragma once

#if LILA_TINTA
#include <optional>

#include "CompanionLegacyTintaReplay.h"
#include "HalTintaReplayStore.h"
#include "HalUnboundCourseMigrationInspection.h"

namespace companion {
struct UnboundCourseReplayItemReport {
  uint32_t compared = 0, committedRecords = 0;
  bool present = false;
};
// Parent freezes replay at the committed boundary and lends exclusive reviewed
// file/workspace owners. A paused review stream may retain only a disjoint prefix
// of the session workspace. No source stream operations run during inspection.
// The report covers items, not undo metadata, counters, marks or publication.
class HalUnboundCourseReplayItemInspection final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCourseReplayItemInspection(HalUnboundCourseMigrationInspection& inspection,
                                       HalUnboundCourseReviewedFile& reviewed, LegacyTintaReplay& replay,
                                       HalTintaReplayStore& store, std::span<uint8_t> scratch, Permission permitted,
                                       void* context)
      : inspection(inspection),
        reviewed(reviewed),
        replay(replay),
        store(store),
        scratch(scratch),
        permitted(permitted),
        context(context) {}
  ~HalUnboundCourseReplayItemInspection() { releaseReaders(); }
  HalUnboundCourseReplayItemInspection(const HalUnboundCourseReplayItemInspection&) = delete;
  HalUnboundCourseReplayItemInspection& operator=(const HalUnboundCourseReplayItemInspection&) = delete;
  bool inspect(const UnboundCourseReviewReservation& input) {
    if (operating) return false;
    ready = false;
    if (!validUnboundCourseReviewReservation(input) || scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &input, sizeof(input)))
      return failure();
    selected = input;
    result = {};
    operating = true;
    cancelled = false;
    bool valid = releaseReaders() && guard();
    const auto* evidence = valid ? inspection.report(selected.intent) : nullptr;
    valid = evidence && evidence->learner.items.catalog.tombstones == 0;
    if (valid) {
      result.present = evidence->learner.items.present;
      result.committedRecords = evidence->learner.items.committedRecords;
      valid = scoped();
    }
    const auto opened =
        valid ? reviewed.open(selected.intent.request, "items.bin") : UnboundReviewedFileResult::Unavailable;
    valid = valid && (result.present ? opened == UnboundReviewedFileResult::Present
                                     : opened == UnboundReviewedFileResult::Missing);
    if (valid && result.present) {
      auto* file = reviewed.borrowed();
      if (file) view.emplace(*file);
      valid = file && view->begin(scratch, allowed, this) && view->header() &&
              view->header()->journalCount == result.committedRecords;
    }
    if (valid) valid = compareItems();
    const bool closed = releaseReaders();
    if (valid && closed && guard()) {
      const auto reopened = reviewed.open(selected.intent.request, "items.bin");
      valid = result.present ? reopened == UnboundReviewedFileResult::Present
                             : reopened == UnboundReviewedFileResult::Missing;
    } else {
      valid = false;
    }
    const bool finalClosed = releaseReaders();
    ready = valid && finalClosed && guard() && scoped() && !cancelled;
    operating = false;
    return ready || failure();
  }
  const UnboundCourseReplayItemReport* report(const UnboundCourseReviewReservation& input) const {
    if (operating || !ready || input != selected) return nullptr;
    operating = true;
    if (!guard() || !scoped() || cancelled) ready = false;
    operating = false;
    return ready && input == selected ? &result : nullptr;
  }
  bool closeReaders() {
    ready = false;
    if (operating) {
      cancelled = true;
      return true;
    }
    return releaseReaders();
  }

 private:
  HalUnboundCourseMigrationInspection& inspection;
  HalUnboundCourseReviewedFile& reviewed;
  LegacyTintaReplay& replay;
  HalTintaReplayStore& store;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  UnboundCourseReviewReservation selected;
  UnboundCourseReplayItemReport result;
  std::optional<HalTintaLegacyItemView> view;
  tinta::core::ItemState observed, expected, actual;
  mutable bool operating = false, ready = false;
  bool cancelled = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseReplayItemInspection*>(context)->guard(); }
  bool scoped() const {
    const auto* evidence = inspection.report(selected.intent);
    return evidence && evidence->learner.items.present == result.present &&
           evidence->learner.items.committedRecords == result.committedRecords && guard() &&
           replay.at(selected, store, result.committedRecords) && guard() &&
           store.matchesCourse(selected.intent.request.original.manifest.logicalIdentity) && guard();
  }
  [[gnu::noinline]] bool compareItems() {
    const auto count = result.present ? view->count() : 0;
    bool previous = false;
    uint32_t key = 0, last = 0;
    for (;;) {
      bool found = false;
      if (!guard() || !store.nextKey(HalTintaReplayStore::Kind::Item, previous, last, key, found) || !guard())
        return false;
      if (!found) break;
      if (!result.present || !key || key == UINT32_MAX || (previous && key <= last) || !compareOne(key, count))
        return false;
      ++result.compared;
      last = key;
      previous = true;
    }
    return result.compared == count && guard();
  }
  [[gnu::noinline]] bool compareOne(uint32_t uid, uint32_t count) {
    bool found = false;
    for (uint32_t slot = 0; slot < count; ++slot) {
      if (!guard() || !view->record(slot, observed) || !observed.uid || observed.uid == UINT32_MAX) return false;
      if (observed.uid == uid) {
        if (found) return false;
        found = true;
        expected = observed;
      }
      if (slot % 32 == 31) vTaskDelay(1);
    }
    return found && guard() && store.item(uid, actual) && guard() && expected == actual;
  }
  bool releaseReaders() {
    view.reset();
    return reviewed.closeReaders();
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound replay item comparison refused");
    return false;
  }
};
}  // namespace companion
#endif
