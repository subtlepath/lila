#pragma once

#include "CompanionDictionaryMemberPublication.h"

namespace companion {
class DictionaryPlanBindingStorage {
 public:
  virtual ~DictionaryPlanBindingStorage() = default;
  // Verify immutable archives and synced binding readback before success.
  virtual bool install(const DictionaryInstallationPlan& plan) = 0;
  // Preserve recovery ownership on failure; cleanup must be retryable.
  virtual bool finalize(const DictionaryInstallationPlan& plan) = 0;
};
// The recovered plan retains the pre-publication canonical proof. All references
// remain live under exclusive installation ownership; no working allocation.
class DictionaryPlanBinding final {
 public:
  DictionaryPlanBinding(DictionaryInstallationParent& parent, DictionaryMemberPublicationStorage& members,
                        DictionaryPlanBindingStorage& bindings)
      : parent(parent), members(members), bindings(bindings) {}
  DictionaryJournalResult install() {
    const auto plan = parent.current();
    if (!plan || !parent.isPhase(TransferPhase::Installing) ||
        (plan->phase != DictionaryInstallationPhase::Publishing && plan->phase != DictionaryInstallationPhase::Bound) ||
        plan->published != plan->extraction.sealed)
      return DictionaryJournalResult::Invalid;
    const auto verified = verify(*plan);
    if (verified != DictionaryJournalResult::Ok) return verified;
    if (!bindings.install(*plan)) return DictionaryJournalResult::IoError;
    return parent.current() == plan && parent.isPhase(TransferPhase::Installing) ? parent.markBound()
                                                                                 : DictionaryJournalResult::Conflict;
  }
  DictionaryJournalResult finalize() {
    const auto plan = parent.current();
    if (!plan || !parent.isPhase(TransferPhase::Committed) ||
        (plan->phase != DictionaryInstallationPhase::Bound && plan->phase != DictionaryInstallationPhase::Committed))
      return DictionaryJournalResult::Invalid;
    const auto verified = verify(*plan);
    if (verified != DictionaryJournalResult::Ok) return verified;
    if (!bindings.finalize(*plan)) return DictionaryJournalResult::IoError;
    return parent.current() == plan && parent.isPhase(TransferPhase::Committed) ? parent.markCommitted()
                                                                                : DictionaryJournalResult::Conflict;
  }

 private:
  DictionaryInstallationParent& parent;
  DictionaryMemberPublicationStorage& members;
  DictionaryPlanBindingStorage& bindings;
  DictionaryJournalResult verify(const DictionaryInstallationPlan& plan) {
    for (unsigned member = 0; member < (plan.extraction.synonyms ? 4u : 3u); ++member) {
      const auto source = members.inspect(plan, member, false);
      const auto target = members.inspect(plan, member, true);
      if (parent.current() != &plan) return DictionaryJournalResult::Conflict;
      if (source == DictionaryMemberPresence::Error || target == DictionaryMemberPresence::Error)
        return DictionaryJournalResult::IoError;
      if (source != DictionaryMemberPresence::Missing || target != DictionaryMemberPresence::Verified)
        return DictionaryJournalResult::Conflict;
    }
    return DictionaryJournalResult::Ok;
  }
};
}  // namespace companion
