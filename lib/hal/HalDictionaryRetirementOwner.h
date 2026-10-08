#pragma once

#include "HalDictionaryCacheStorage.h"
#include "HalDictionaryRetirementSession.h"

namespace companion {
// Checked session allocation outside the task stack. Providers and scratch stay
// exclusively borrowed; durable transfer authorization survives journal removal.
class HalDictionaryRetirementOwner final {
 public:
  HalDictionaryRetirementOwner(const Transfer& transfer, HalDictionaryExtractionJournalStorage& extraction,
                               HalDictionaryExtractionJournalStorage& installation,
                               HalDictionaryExtractionJournalStorage& proofs, HalCompanionFileLookup& lookup,
                               HalDictionaryDestinationLookup& destination,
                               HalDictionaryMemberVerification& verification, std::span<uint8_t> proofScratch,
                               std::span<uint8_t> wireScratch, InventoryHashProgress progress = nullptr,
                               void* context = nullptr)
      : transfer(transfer),
        scratchValid(validScratch(proofScratch, wireScratch)),
        retirement(proofs, proofScratch, transfer),
        cache(progress, context),
        bindings(cache, wireScratch, progress, context),
        members(destination, verification, guard, this, progress, context),
        installed(members, bindings, guard, this),
        session(retirement, installed, extraction, installation, proofs, lookup, guard, this, wireScratch) {}
  DictionaryJournalResult begin(const DictionaryInstallationParent& parent) {
    if (!scratchValid) return invalidScratch();
    const auto plan = parent.current();
    if (!plan || !installed.verify(*plan)) {
      LOG_ERR("COMPANION", "Dictionary retirement owner cannot verify committed installation");
      return DictionaryJournalResult::Conflict;
    }
    return session.begin(parent);
  }
  DictionaryJournalResult resume() { return scratchValid ? session.resume() : invalidScratch(); }
  const DictionaryInstallationPlan* current() const { return retirement.current(); }

 private:
  const Transfer& transfer;
  const bool scratchValid;
  DictionaryRetirementJournal retirement;
  HalDictionaryCacheStorage cache;
  HalDictionaryBindings bindings;
  HalDictionaryMemberPublicationStorage members;
  HalDictionaryInstalledVerification installed;
  HalDictionaryRetirementSession session;
  static bool validScratch(std::span<uint8_t> proof, std::span<uint8_t> wire) {
    if (proof.size() < DICTIONARY_RETIREMENT_PROOF_SIZE || wire.size() < DICTIONARY_RETIREMENT_PROOF_SIZE) return false;
    const auto a = reinterpret_cast<uintptr_t>(proof.data());
    const auto b = reinterpret_cast<uintptr_t>(wire.data());
    return a <= b ? b - a >= proof.size() : a - b >= wire.size();
  }
  static DictionaryJournalResult invalidScratch() {
    LOG_ERR("COMPANION", "Dictionary retirement owner needs disjoint proof and wire scratch");
    return DictionaryJournalResult::Invalid;
  }
  static bool guard(void* context, const DictionaryInstallationPlan& proof) {
    return dictionaryRetirementProofMatchesTransfer(proof,
                                                    static_cast<HalDictionaryRetirementOwner*>(context)->transfer);
  }
};
}  // namespace companion
