#pragma once

#include "CompanionDictionaryExtractionParent.h"
#include "CompanionDictionaryInstallationJournal.h"

namespace companion {
// Plans originate from validated member/canonical proofs. Transfer and extraction
// references must remain live under the serialized installation controller.
class DictionaryInstallationParent final {
 public:
  DictionaryInstallationParent(DictionaryInstallationJournal& journal, const DictionaryExtractionParent& extraction,
                               const Transfer& transfer)
      : journal(journal), extraction(extraction), transfer(transfer) {}
  DictionaryJournalResult begin(const DictionaryInstallationPlan& initial) {
    if (!canBegin(initial)) return reject();
    return journal.begin(initial);
  }
  bool canBegin(const DictionaryInstallationPlan& initial) const { return matches(initial) && !committed(); }
  DictionaryJournalResult recover(const DictionaryInstallationPlan& expected) {
    if (!matches(expected)) return reject();
    const auto result = journal.recover(expected);
    if (result == DictionaryJournalResult::Ok && !current()) return reject();
    return result;
  }
  DictionaryJournalResult recover() {
    const auto result = journal.recover(transfer);
    if (result == DictionaryJournalResult::Ok && !current()) return reject();
    return result;
  }
  const DictionaryInstallationPlan* current() const {
    const auto plan = journal.current();
    if (!plan || !matches(*plan)) return nullptr;
    const auto phase = transfer.current()->phase;
    if (phase == TransferPhase::Committed)
      return plan->phase == DictionaryInstallationPhase::Bound || plan->phase == DictionaryInstallationPhase::Committed
                 ? plan
                 : nullptr;
    if (phase == TransferPhase::Installing)
      return plan->phase != DictionaryInstallationPhase::Committed ? plan : nullptr;
    return plan->phase == DictionaryInstallationPhase::Prepared ? plan : nullptr;
  }
  DictionaryJournalResult startPublishing() { return installing() ? journal.startPublishing() : reject(); }
  DictionaryJournalResult recordPublished(unsigned member) {
    return installing() ? journal.recordPublished(member) : reject();
  }
  DictionaryJournalResult markBound() { return installing() ? journal.markBound() : reject(); }
  DictionaryJournalResult markCommitted() { return current() && committed() ? journal.markCommitted() : reject(); }
  bool isPhase(TransferPhase phase) const { return current() && transfer.current()->phase == phase; }

 private:
  DictionaryInstallationJournal& journal;
  const DictionaryExtractionParent& extraction;
  const Transfer& transfer;
  bool committed() const { return transfer.current() && transfer.current()->phase == TransferPhase::Committed; }
  bool installing() const { return current() && transfer.current()->phase == TransferPhase::Installing; }
  bool matches(const DictionaryInstallationPlan& plan) const {
    const auto receipt = extraction.current();
    const auto state = transfer.current();
    const auto manifest = transfer.contentManifest();
    return receipt && state && manifest && validDictionaryInstallationPlan(plan) && plan.extraction == *receipt &&
           plan.archives.original == *manifest && extraction.matchesArchive(*manifest) &&
           extraction.isPhase(state->phase) && state->transaction == receipt->transaction &&
           state->storageGeneration == receipt->generation && state->contentHash == receipt->archiveHash &&
           state->durableOffset == state->length && transfer.destination() == std::string_view(plan.base.data());
  }
  DictionaryJournalResult reject() {
    journal.invalidate();
    return DictionaryJournalResult::Conflict;
  }
};
}  // namespace companion
