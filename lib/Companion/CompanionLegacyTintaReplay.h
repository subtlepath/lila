#pragma once

#include "CompanionLegacyTintaEventCursor.h"
#include "CompanionLegacyTintaMutation.h"
#include "CompanionTintaReplayReducer.h"
#include "CompanionUnboundCourseReviewReservation.h"

namespace companion {
// Parent starts an empty disposable store, excludes its writers and retains this
// owner off stack. Failure poisons the candidate; restart requires an empty store.
// Reconstructs prior states, not snapshot agreement or migration provenance.
class LegacyTintaReplay final {
 public:
  using Permission = bool (*)(void*);
  LegacyTintaReplay(TintaReplayStore& store, Permission permitted, void* context)
      : store(store), permitted(permitted), context(context) {}
  bool begin(const UnboundCourseReviewReservation& input, const TintaSchedulerConfiguration& configuration) {
    if (operating) return false;
    ready = false;
    std::array<uint8_t, 6> encoded{};
    if (!permitted || !validUnboundCourseReviewReservation(input) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &input, sizeof(input)) ||
        !encodeTintaConfiguration(configuration, encoded))
      return false;
    selected = input;
    this->configuration = configuration;
    operating = true;
    cancelled = hasUndo = false;
    progress = undoProgress = {};
    ready = guard() && cursor.begin(selected.first(), selected.records) && guard();
    operating = false;
    return ready;
  }
  bool apply(const UnboundCourseReviewReservation& input, uint32_t index, const LegacyTintaEntry& source,
             tinta::core::ItemState& outputPrior) {
    if (operating || !ready || input != selected) return false;
    if (course_baseline_detail::overlaps(this, sizeof(*this), &source, sizeof(source)) ||
        course_baseline_detail::overlaps(this, sizeof(*this), &outputPrior, sizeof(outputPrior)) ||
        course_baseline_detail::overlaps(&source, sizeof(source), &outputPrior, sizeof(outputPrior)) ||
        course_baseline_detail::overlaps(&input, sizeof(input), &outputPrior, sizeof(outputPrior)))
      return false;
    entry = source;
    operating = true;
    proposed = cursor;
    bool valid = guard() && proposed.assign(index, entry, identities) && proposed.events() <= selected.events &&
                 store.item(entry.uid, before) && guard() && before.uid == entry.uid &&
                 mapLegacyTintaMutation(entry, before, selected.intent.request.original.manifest.logicalIdentity,
                                        configuration, identities.undoTarget, mutation);
    if (valid) {
      after = before;
      if (entry.operation == LegacyTintaOperation::Undo) {
        valid = hasUndo && before == undoAfter && guard() && store.putItem(undoBefore) && guard() &&
                store.putDay(undoDay, undoTotals) && guard();
      } else {
        for (uint8_t at = 0; valid && at < mutation.count; ++at)
          valid = applyTintaItemReplay(mutation.bodies[at], entry.studyDay, scheduler, after, counts);
        if (valid && entry.operation == LegacyTintaOperation::Review) valid = reviewTotals();
        valid = valid && guard() && store.putItem(after) && guard();
        if (valid && entry.operation == LegacyTintaOperation::Review)
          valid = store.putDay(entry.studyDay, totals) && guard();
      }
    }
    valid = valid && guard() && input == selected;
    if (valid) {
      if (entry.operation == LegacyTintaOperation::Review) {
        undoBefore = before;
        undoAfter = after;
        undoTotals = priorTotals;
        undoDay = entry.studyDay;
        undoProgress = progress;
        if (progress.day != entry.studyDay) progress = {entry.studyDay, 0, 0};
        if (counts.newItem) progress.newItems = increment(progress.newItems);
        if (counts.review) progress.reviews = increment(progress.reviews);
      } else if (entry.operation == LegacyTintaOperation::Undo) {
        progress = undoProgress;
      }
      hasUndo = entry.operation == LegacyTintaOperation::Review;
      cursor = proposed;
      outputPrior = before;
    } else {
      ready = false;
    }
    operating = false;
    return valid;
  }
  bool complete(const UnboundCourseReviewReservation& input) const {
    if (operating || !ready || input != selected) return false;
    operating = true;
    if (!guard()) ready = false;
    const bool valid = ready && input == selected && cursor.complete() && cursor.events() == selected.events;
    operating = false;
    return valid;
  }
  bool at(const UnboundCourseReviewReservation& input, const TintaReplayStore& candidate, uint32_t records) const {
    if (operating || !ready || input != selected || &candidate != &store) return false;
    operating = true;
    if (!guard()) ready = false;
    const bool valid = ready && input == selected && cursor.records() == records && records <= selected.records;
    operating = false;
    return valid;
  }
  bool matchesProgressHeader(const UnboundCourseReviewReservation& input, const TintaReplayStore& candidate,
                             const tinta::core::ProgressHeader& header) const {
    if (!at(input, candidate, header.journalCount)) return false;
    operating = true;
    if (!guard()) ready = false;
    const bool valid =
        ready && input == selected && header.statDay == progress.day && header.statNew == progress.newItems &&
        header.statReviews == progress.reviews && header.undoValid == hasUndo &&
        (!hasUndo || (header.undoBefore == undoBefore && header.undoStatDay == undoProgress.day &&
                      header.undoStatNew == undoProgress.newItems && header.undoStatReviews == undoProgress.reviews));
    operating = false;
    return valid;
  }
  void close() {
    if (operating) cancelled = true;
    ready = false;
  }

 private:
  TintaReplayStore& store;
  Permission permitted;
  void* context;
  UnboundCourseReviewReservation selected;
  TintaSchedulerConfiguration configuration;
  LegacyTintaEventCursor cursor, proposed;
  LegacyTintaEventIdentities identities;
  LegacyTintaEntry entry;
  TintaProgressMutation mutation;
  tinta::core::Fsrs scheduler;
  tinta::core::ItemState before, after, undoBefore, undoAfter;
  TintaReplayDay totals, priorTotals, undoTotals;
  TintaReplayCounts counts;
  struct ProgressCounters {
    uint16_t day = 0, newItems = 0, reviews = 0;
  } progress, undoProgress;
  static uint16_t increment(uint16_t value) { return value == UINT16_MAX ? value : static_cast<uint16_t>(value + 1); }
  uint16_t undoDay = 0;
  mutable bool operating = false, ready = false;
  bool cancelled = false, hasUndo = false;
  bool guard() const { return !cancelled && permitted && permitted(context) && !cancelled; }
  [[gnu::noinline]] bool reviewTotals() {
    if (!guard() || !store.day(entry.studyDay, priorTotals) || !guard()) return false;
    totals = priorTotals;
    const uint32_t response = static_cast<uint32_t>(entry.responseQuarterSeconds) * 250;
    if (totals.gradedReviews == UINT32_MAX || (counts.newItem && totals.newItems == UINT32_MAX) ||
        (counts.review && totals.reviews == UINT32_MAX) || (counts.correct && totals.correctReviews == UINT32_MAX) ||
        response > UINT64_MAX - totals.responseMilliseconds)
      return false;
    ++totals.gradedReviews;
    totals.newItems += counts.newItem;
    totals.reviews += counts.review;
    totals.correctReviews += counts.correct;
    totals.responseMilliseconds += response;
    return true;
  }
};
}  // namespace companion
