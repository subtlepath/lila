#pragma once

#include "CompanionJournalReplayOrder.h"
#include "CompanionReadingBody.h"

namespace companion {
// Retain off-stack. Caller freezes journal/index and owns disposable visit marks.
// Concurrent distinct anchors stay in journal authority for explicit resolution.
class ReadingPositionResolution final {
 public:
  TintaJournalResult run(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks,
                         const Digest& edition, ReadingAnchor& output) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    if (!tinta_body_detail::nonzero(edition)) return TintaJournalResult::Invalid;
    const auto count = journal.count();
    const auto validated = validation.validate(journal, index);
    if (validated != TintaJournalResult::Ok) return validated;
    if (!marks.reset(count)) return TintaJournalResult::IoError;
    bool selected = false, conflict = false;
    for (uint32_t remaining = count; remaining; --remaining) {
      const auto record = remaining - 1;
      bool marked = false;
      if (!marks.visited(record, marked)) return TintaJournalResult::IoError;
      const auto read = journal.read(record);
      if (read != TintaJournalResult::Ok) return read;
      event = journal.event();
      const bool matching = event.kind == EventKind::ReadingPosition && event.resource == edition;
      if (matching && !marked) {
        ReadingAnchor value;
        if (!decodeReadingAnchor(journal.body(), value)) return TintaJournalResult::Corrupt;
        if (!selected) {
          anchor = value;
          selected = true;
        } else if (anchor != value) {
          conflict = true;
        }
      }
      if (matching || marked) {
        for (unsigned at = 0; at < event.ancestorCount; ++at) {
          const auto result = markParent(index, marks, event.ancestors[at], record);
          if (result != TintaJournalResult::Ok) return result;
        }
        if (event.identity.sequence > 1) {
          predecessor = event.identity;
          --predecessor.sequence;
          const auto result = markParent(index, marks, predecessor, record);
          if (result != TintaJournalResult::Ok) return result;
        }
      }
      if (journal.count() != count) return TintaJournalResult::Conflict;
    }
    if (conflict) return TintaJournalResult::Conflict;
    if (!selected) return TintaJournalResult::Unavailable;
    output = anchor;
    return TintaJournalResult::Ok;
  }

 private:
  static TintaJournalResult markParent(JournalIdentityIndex& index, JournalReplayVisits& marks,
                                       const EventIdentity& identity, uint32_t child) {
    uint32_t record = 0;
    const auto found = index.find(identity, record);
    if (found == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (found != JournalIdentityLookup::Found || record >= child) return TintaJournalResult::Corrupt;
    return marks.mark(record) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }
  JournalCausalValidation validation;
  SyncEvent event;
  EventIdentity predecessor;
  ReadingAnchor anchor;
};
}  // namespace companion
