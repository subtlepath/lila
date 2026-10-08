#pragma once

#include "CompanionJournalReplayOrder.h"

namespace companion {
// Retain off-stack; caller freezes the journal/index and owns disposable SD marks.
// Heads are streamed without a RAM-sized limit on independent origins.
class JournalKnowledgeHeads {
 public:
  TintaJournalResult begin(TintaJournal& source, JournalIdentityIndex& identities, JournalReplayVisits& visits) {
    ready = false;
    const auto result = validation.validate(source, identities);
    if (result != TintaJournalResult::Ok) return result;
    count = source.count();
    if (!visits.reset(count)) return TintaJournalResult::IoError;
    for (uint32_t at = 0; at < count; ++at) {
      const auto read = source.read(at);
      if (read != TintaJournalResult::Ok) return read;
      identity = source.event().identity;
      const auto parentCount = source.event().ancestorCount;
      std::copy_n(source.event().ancestors.begin(), parentCount, parents.begin());
      for (unsigned parent = 0; parent < parentCount; ++parent) {
        const auto marked = markParent(identities, visits, parents[parent], at);
        if (marked != TintaJournalResult::Ok) return marked;
      }
      if (identity.sequence > 1) {
        --identity.sequence;
        const auto marked = markParent(identities, visits, identity, at);
        if (marked != TintaJournalResult::Ok) return marked;
      }
      if (source.count() != count) return TintaJournalResult::Conflict;
    }
    journal = &source;
    marks = &visits;
    cursor = 0;
    ready = true;
    return TintaJournalResult::Ok;
  }
  TintaJournalResult next(EventIdentity& output, bool& complete) {
    if (!ready) return TintaJournalResult::Unavailable;
    if (journal->count() != count) return fail(TintaJournalResult::Conflict);
    while (cursor < count) {
      const auto record = cursor++;
      bool hasChild = false;
      if (!marks->visited(record, hasChild)) return fail(TintaJournalResult::IoError);
      if (hasChild) continue;
      const auto read = journal->read(record);
      if (read != TintaJournalResult::Ok) return fail(read);
      if (journal->count() != count) return fail(TintaJournalResult::Conflict);
      output = journal->event().identity;
      complete = false;
      return TintaJournalResult::Ok;
    }
    complete = true;
    return TintaJournalResult::Ok;
  }

 private:
  static TintaJournalResult markParent(JournalIdentityIndex& index, JournalReplayVisits& visits,
                                       const EventIdentity& parent, uint32_t child) {
    uint32_t record = 0;
    const auto lookup = index.find(parent, record);
    if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (lookup != JournalIdentityLookup::Found || record >= child) return TintaJournalResult::Corrupt;
    return visits.mark(record) ? TintaJournalResult::Ok : TintaJournalResult::IoError;
  }
  TintaJournalResult fail(TintaJournalResult result) {
    ready = false;
    return result;
  }
  JournalCausalValidation validation;
  std::array<EventIdentity, MAX_ANCESTORS> parents{};
  EventIdentity identity{};
  TintaJournal* journal = nullptr;
  JournalReplayVisits* marks = nullptr;
  uint32_t count = 0;
  uint32_t cursor = 0;
  bool ready = false;
};
}  // namespace companion
