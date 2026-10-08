#pragma once

#include "CompanionTintaJournal.h"

namespace companion {
enum class JournalIdentityLookup { Found, Missing, IoError };
// Caller supplies a stable index of the exact committed journal and excludes writers.
class JournalIdentityIndex {
 public:
  virtual ~JournalIdentityIndex() = default;
  virtual JournalIdentityLookup find(const EventIdentity& identity, uint32_t& record) = 0;
};
// Session-owned metadata avoids copying a full event onto the small task stack.
// Persistent records must be in causal order; exchange orders deliveries before admission.
class JournalCausalValidation {
 public:
  TintaJournalResult validate(TintaJournal& journal, JournalIdentityIndex& index) {
    if (!journal.available()) return TintaJournalResult::Unavailable;
    const auto count = journal.count();
    for (uint32_t at = 0; at < count; ++at) {
      auto result = journal.read(at);
      if (result != TintaJournalResult::Ok) return result;
      identity = journal.event().identity;
      parentCount = journal.event().ancestorCount;
      std::copy_n(journal.event().ancestors.begin(), parentCount, parents.begin());
      uint32_t found = 0;
      auto lookup = index.find(identity, found);
      if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
      if (lookup != JournalIdentityLookup::Found || found != at) return TintaJournalResult::Corrupt;
      for (unsigned parent = 0; parent < parentCount; ++parent) {
        result = checkParent(journal, index, parents[parent], at);
        if (result != TintaJournalResult::Ok) return result;
      }
      if (identity.sequence > 1) {
        predecessor = identity;
        --predecessor.sequence;
        result = checkParent(journal, index, predecessor, at);
        if (result != TintaJournalResult::Ok) return result;
      }
      if (journal.count() != count) return TintaJournalResult::Conflict;
    }
    return TintaJournalResult::Ok;
  }

 private:
  static TintaJournalResult checkParent(TintaJournal& journal, JournalIdentityIndex& index, const EventIdentity& parent,
                                        uint32_t child) {
    uint32_t found = 0;
    const auto lookup = index.find(parent, found);
    if (lookup == JournalIdentityLookup::IoError) return TintaJournalResult::IoError;
    if (lookup != JournalIdentityLookup::Found || found >= child) return TintaJournalResult::Corrupt;
    const auto result = journal.read(found);
    if (result != TintaJournalResult::Ok) return result;
    return journal.event().identity == parent ? TintaJournalResult::Ok : TintaJournalResult::Corrupt;
  }
  EventIdentity identity{}, predecessor{};
  std::array<EventIdentity, MAX_ANCESTORS> parents{};
  unsigned parentCount = 0;
};
}  // namespace companion
