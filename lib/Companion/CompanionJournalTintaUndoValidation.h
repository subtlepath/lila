#pragma once

#include "CompanionJournalReplayOrder.h"

namespace companion {
// Session-owned bodies; caller audits closure and freezes both journal/index files.
class JournalTintaUndoValidation {
 public:
  TintaJournalResult validate(TintaJournal& journal, JournalIdentityIndex& index,
                              JournalReplayVisits* exclusions = nullptr) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    const auto count = journal.count();
    if (exclusions && !exclusions->reset(count)) return TintaJournalResult::IoError;
    for (uint32_t at = 0; at < count; ++at) {
      auto result = journal.read(at);
      if (result != TintaJournalResult::Ok) return result;
      if (journal.event().kind != EventKind::UndoReview) continue;
      if (!decodeTintaBody(journal.body(), undo)) return TintaJournalResult::Corrupt;
      uint32_t target = 0;
      const auto lookup = index.find(undo.undoTarget, target);
      if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
      if (lookup != JournalIdentityLookup::Found || target >= at) return TintaJournalResult::Corrupt;
      result = journal.read(target);
      if (result != TintaJournalResult::Ok) return result;
      if (journal.event().identity != undo.undoTarget || journal.event().kind != EventKind::Review ||
          !decodeTintaBody(journal.body(), review) || review.course != undo.course || review.uid != undo.uid)
        return TintaJournalResult::Corrupt;
      if (exclusions && !exclusions->mark(target)) return TintaJournalResult::IoError;
    }
    return journal.count() == count ? TintaJournalResult::Ok : TintaJournalResult::Conflict;
  }

 private:
  TintaBody undo{}, review{};
};
}  // namespace companion
