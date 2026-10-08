#pragma once

#include "CompanionJournalReplayOrder.h"

namespace companion {
// Retain off-stack; source/index are frozen and marks are disposable. Backward
// traversal works because validated dependencies always precede their children.
class JournalCausalRelation final {
 public:
  TintaJournalResult precedes(TintaJournal& journal, JournalIdentityIndex& index, JournalReplayVisits& marks,
                              uint32_t ancestor, uint32_t descendant, bool& output) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    const auto count = journal.count();
    if (ancestor >= count || descendant >= count) return TintaJournalResult::Invalid;
    auto result = validation.validate(journal, index);
    if (result != TintaJournalResult::Ok) return result;
    if (ancestor >= descendant) {
      output = false;
      return TintaJournalResult::Ok;
    }
    if (!marks.reset(count) || !marks.mark(descendant)) return TintaJournalResult::IoError;
    for (uint32_t at = descendant; at > ancestor; --at) {
      bool marked = false;
      if (!marks.visited(at, marked)) return TintaJournalResult::IoError;
      if (!marked) continue;
      result = journal.read(at);
      if (result != TintaJournalResult::Ok) return result;
      event = journal.event();
      for (unsigned parent = 0; parent < event.ancestorCount; ++parent) {
        result = markParent(index, marks, event.ancestors[parent], at);
        if (result != TintaJournalResult::Ok) return result;
      }
      if (event.identity.sequence > 1) {
        predecessor = event.identity;
        --predecessor.sequence;
        result = markParent(index, marks, predecessor, at);
        if (result != TintaJournalResult::Ok) return result;
      }
      if (journal.count() != count) return TintaJournalResult::Conflict;
    }
    bool related = false;
    if (!marks.visited(ancestor, related)) return TintaJournalResult::IoError;
    if (journal.count() != count) return TintaJournalResult::Conflict;
    output = related;
    return TintaJournalResult::Ok;
  }

 private:
  static TintaJournalResult markParent(JournalIdentityIndex& index, JournalReplayVisits& marks,
                                       const EventIdentity& identity, uint32_t child) {
    uint32_t record = 0;
    const auto lookup = index.find(identity, record);
    if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (lookup != JournalIdentityLookup::Found || record >= child) return TintaJournalResult::Corrupt;
    return marks.mark(record) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }
  JournalCausalValidation validation;
  SyncEvent event;
  EventIdentity predecessor;
};
}  // namespace companion
