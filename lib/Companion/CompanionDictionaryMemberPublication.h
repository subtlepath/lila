#pragma once

#include "CompanionDictionaryInstallationParent.h"

namespace companion {
enum class DictionaryMemberPresence { Missing, Verified, Conflict, Error };
class DictionaryMemberPublicationStorage {
 public:
  virtual ~DictionaryMemberPublicationStorage() = default;
  // Verified requires exact receipt length/SHA and an unambiguous regular file.
  virtual DictionaryMemberPresence inspect(const DictionaryInstallationPlan& plan, unsigned member, bool installed) = 0;
  // Exclusive destination ownership; never replace an existing file.
  virtual bool move(const DictionaryInstallationPlan& plan, unsigned member) = 0;
};
// Serialized owner retains the plan, stage files and destination reservation.
// This publishes into a reserved destination; replacement/rollback is separate.
class DictionaryMemberPublication final {
 public:
  DictionaryMemberPublication(DictionaryInstallationParent& parent, DictionaryMemberPublicationStorage& storage)
      : parent(parent), storage(storage) {}
  DictionaryJournalResult publish() {
    auto plan = parent.current();
    if (!plan || (plan->phase != DictionaryInstallationPhase::Prepared &&
                  plan->phase != DictionaryInstallationPhase::Publishing))
      return DictionaryJournalResult::Invalid;
    moved.fill(false);
    bool earlierUnpublished = false;
    for (unsigned member : ORDER) {
      if (member == 3 && !plan->extraction.synonyms) continue;
      const auto source = storage.inspect(*plan, member, false);
      const auto target = storage.inspect(*plan, member, true);
      if (parent.current() != plan) return DictionaryJournalResult::Conflict;
      if (source == DictionaryMemberPresence::Error || target == DictionaryMemberPresence::Error)
        return DictionaryJournalResult::IoError;
      const bool published = plan->published & (1u << member);
      moved[member] = source == DictionaryMemberPresence::Missing && target == DictionaryMemberPresence::Verified;
      if (!published && moved[member] && earlierUnpublished) return DictionaryJournalResult::Conflict;
      if (published ? !moved[member]
                    : !(source == DictionaryMemberPresence::Verified && target == DictionaryMemberPresence::Missing) &&
                          !(plan->phase == DictionaryInstallationPhase::Publishing && moved[member]))
        return DictionaryJournalResult::Conflict;
      if (!published) earlierUnpublished = true;
    }
    auto result = parent.startPublishing();
    if (result != DictionaryJournalResult::Ok) return result;
    for (unsigned member : ORDER) {
      plan = parent.current();
      if (!plan) return DictionaryJournalResult::Conflict;
      if ((member == 3 && !plan->extraction.synonyms) || (plan->published & (1u << member))) continue;
      if (!moved[member] && !storage.move(*plan, member)) return DictionaryJournalResult::IoError;
      if (parent.current() != plan) return DictionaryJournalResult::Conflict;
      if (storage.inspect(*plan, member, false) != DictionaryMemberPresence::Missing ||
          storage.inspect(*plan, member, true) != DictionaryMemberPresence::Verified)
        return DictionaryJournalResult::IoError;
      if (parent.current() != plan) return DictionaryJournalResult::Conflict;
      result = parent.recordPublished(member);
      if (result != DictionaryJournalResult::Ok) return result;
    }
    return DictionaryJournalResult::Ok;
  }

 private:
  static constexpr unsigned ORDER[] = {2, 1, 0, 3};
  DictionaryInstallationParent& parent;
  DictionaryMemberPublicationStorage& storage;
  std::array<bool, 4> moved{};
};
}  // namespace companion
