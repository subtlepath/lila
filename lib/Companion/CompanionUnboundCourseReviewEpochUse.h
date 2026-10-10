#pragma once

#include "CompanionTintaJournal.h"
#include "CompanionUnboundCourseReviewReservation.h"

namespace companion {
struct UnboundCourseReviewEpochUse {
  uint32_t matchedEvents = 0, ancestorReferences = 0;
  uint64_t greatestSequence = 0;
  bool foreignGeneration = false, outsideReviewRange = false;
  bool operator==(const UnboundCourseReviewEpochUse&) const = default;
};
// Caller lends an exclusive, validated immutable journal/workspace and reservation.
// This scans one journal; zero matches never proves global epoch ownership.
// Matching identities require body/provenance verification before recovery.
// Callbacks must preserve the journal loan; permission callbacks also provide yielding.
inline bool inspectUnboundCourseReviewEpochUse(TintaJournal& journal, const UnboundCourseReviewReservation& reservation,
                                               UnboundCourseReviewEpochUse& output, bool (*permitted)(void*),
                                               void* context,
                                               bool (*verifyEvent)(void*, const SyncEvent&,
                                                                   std::span<const uint8_t>) = nullptr) {
  if (!permitted || !validUnboundCourseReviewReservation(reservation) || !journal.available() ||
      course_baseline_detail::overlaps(&reservation, sizeof(reservation), &output, sizeof(output)) ||
      course_baseline_detail::overlaps(&journal, sizeof(journal), &output, sizeof(output)) || !permitted(context))
    return false;
  const auto count = journal.count();
  UnboundCourseReviewEpochUse result;
  for (uint32_t index = 0; index < count; ++index) {
    if (!permitted(context) || journal.read(index) != TintaJournalResult::Ok) return false;
    const auto& event = journal.event();
    if (event.identity.origin == reservation.intent.reader && event.identity.epoch == reservation.epoch) {
      if (verifyEvent && (!verifyEvent(context, event, journal.body()) || !permitted(context) || !journal.available() ||
                          journal.count() != count))
        return false;
      ++result.matchedEvents;
      result.greatestSequence = std::max(result.greatestSequence, event.identity.sequence);
      result.foreignGeneration |= event.storageGeneration != reservation.intent.request.original.generation;
      result.outsideReviewRange |= event.identity.sequence > reservation.events;
    }
    for (unsigned ancestor = 0; ancestor < event.ancestorCount; ++ancestor) {
      const auto& identity = event.ancestors[ancestor];
      if (identity.origin != reservation.intent.reader || identity.epoch != reservation.epoch) continue;
      ++result.ancestorReferences;
      result.greatestSequence = std::max(result.greatestSequence, identity.sequence);
      result.outsideReviewRange |= identity.sequence > reservation.events;
    }
    if (!permitted(context) || !journal.available() || journal.count() != count) return false;
  }
  if (!permitted(context) || !journal.available() || journal.count() != count) return false;
  output = result;
  return true;
}
}  // namespace companion
