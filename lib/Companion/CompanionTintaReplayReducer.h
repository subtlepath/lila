#pragma once

#include "CompanionTintaItemReplay.h"
#include "CompanionTintaJournal.h"

namespace companion {
struct TintaReplayDay {
  uint32_t newItems = 0, reviews = 0, gradedReviews = 0, correctReviews = 0;
  uint64_t responseMilliseconds = 0;
};
// Writes affect disposable candidate files only; any failure discards the candidate.
class TintaReplayStore {
 public:
  virtual ~TintaReplayStore() = default;
  // Missing items/days return a fresh item/zero totals, respectively.
  virtual bool item(uint32_t uid, tinta::core::ItemState& value) = 0;
  virtual bool putItem(const tinta::core::ItemState& value) = 0;
  virtual bool day(uint16_t day, TintaReplayDay& value) = 0;
  virtual bool putDay(uint16_t day, const TintaReplayDay& value) = 0;
  virtual bool completion(EventKind kind, uint32_t uid, bool enabled) = 0;
};
// Session-owned reducer; caller supplies audited, ordered events with undo marks.
class TintaReplayReducer {
 public:
  TintaReplayReducer(TintaReplayStore& store, const Identity& course) : store(store), course(course) {
    ready = tinta_body_detail::nonzero(course);
  }
  TintaJournalResult apply(const SyncEvent& event, std::span<const uint8_t> bytes, bool undoneReview = false) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (!tinta_body_detail::size(event.kind))
      return undoneReview ? fail(TintaJournalResult::Invalid) : TintaJournalResult::Ok;
    if (!decodeTintaBody(bytes, body) || body.kind != event.kind || event.studyDay > UINT16_MAX ||
        (undoneReview && event.kind != EventKind::Review))
      return fail(TintaJournalResult::Invalid);
    if (body.course != course) return TintaJournalResult::Ok;
    if (body.kind == EventKind::UndoReview) return fail(TintaJournalResult::Invalid);
    if (body.kind == EventKind::LessonComplete || body.kind == EventKind::ReadingComplete)
      return store.completion(body.kind, body.uid, body.enabled) ? TintaJournalResult::Ok
                                                                 : fail(TintaJournalResult::IoError);
    tinta::core::ItemState item;
    if (!store.item(body.uid, item) || item.uid != body.uid) return fail(TintaJournalResult::IoError);
    if (undoneReview) return store.putItem(item) ? TintaJournalResult::Ok : fail(TintaJournalResult::IoError);
    TintaReplayCounts counts;
    if (!applyTintaItemReplay(body, event.studyDay, scheduler, item, counts)) return fail(TintaJournalResult::Invalid);
    if (body.kind != EventKind::Review)
      return store.putItem(item) ? TintaJournalResult::Ok : fail(TintaJournalResult::IoError);
    TintaReplayDay totals;
    const auto day = static_cast<uint16_t>(event.studyDay);
    if (!store.day(day, totals)) return fail(TintaJournalResult::IoError);
    if (totals.gradedReviews == UINT32_MAX || (counts.newItem && totals.newItems == UINT32_MAX) ||
        (counts.review && totals.reviews == UINT32_MAX) || (counts.correct && totals.correctReviews == UINT32_MAX) ||
        body.responseMilliseconds > UINT64_MAX - totals.responseMilliseconds)
      return fail(TintaJournalResult::Exhausted);
    ++totals.gradedReviews;
    totals.newItems += counts.newItem;
    totals.reviews += counts.review;
    totals.correctReviews += counts.correct;
    totals.responseMilliseconds += body.responseMilliseconds;
    if (!store.putItem(item) || !store.putDay(day, totals)) return fail(TintaJournalResult::IoError);
    return TintaJournalResult::Ok;
  }

 private:
  TintaJournalResult fail(TintaJournalResult result) {
    ready = false;
    return result;
  }
  TintaReplayStore& store;
  Identity course;
  TintaBody body;
  tinta::core::Fsrs scheduler;
  bool ready = false;
};
}  // namespace companion
