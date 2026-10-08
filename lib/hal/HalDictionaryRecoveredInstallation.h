#pragma once

#include "HalDictionaryPublicationSession.h"

namespace companion {
// Retained outside the task stack. Transfer, serialized providers and disjoint
// journal/wire scratch remain borrowed until this owner is destroyed.
class HalDictionaryRecoveredInstallation final {
 public:
  HalDictionaryRecoveredInstallation(const Transfer& transfer, const TransferState& state,
                                     const ContentManifest& manifest, const Identity& generation,
                                     DictionaryExtractionJournalStorage& extractionStorage,
                                     DictionaryExtractionJournalStorage& installationStorage,
                                     std::span<uint8_t> extractionScratch, std::span<uint8_t> installationScratch,
                                     HalDictionaryDestinationLookup& lookup,
                                     HalDictionaryMemberVerification& verification, std::span<uint8_t> wireScratch,
                                     InventoryHashProgress progress = nullptr, void* context = nullptr)
      : transfer(transfer),
        state(state),
        manifest(manifest),
        extractionJournal(extractionStorage, extractionScratch),
        extraction(extractionJournal, state, manifest, generation),
        installationJournal(installationStorage, installationScratch),
        parent(installationJournal, extraction, transfer),
        publication(parent, lookup, verification, wireScratch, progress, context) {}
  DictionaryJournalResult recover() {
    ready = false;
    installationJournal.invalidate();
    extractionJournal.invalidate();
    if (transfer.current() != &state || transfer.contentManifest() != &manifest ||
        (state.phase != TransferPhase::Installing && state.phase != TransferPhase::Committed))
      return fail(DictionaryJournalResult::Conflict, "transfer context");
    const auto receipt = extraction.recover();
    if (receipt != DictionaryJournalResult::Ok) return fail(receipt, "extraction receipt");
    const auto plan = parent.recover();
    if (plan != DictionaryJournalResult::Ok) return fail(plan, "installation plan");
    ready = true;
    return DictionaryJournalResult::Ok;
  }
  DictionaryJournalResult install() {
    const auto result = recover();
    return result == DictionaryJournalResult::Ok ? publication.install() : result;
  }
  DictionaryJournalResult finalize() {
    const auto result = recover();
    return result == DictionaryJournalResult::Ok ? publication.finalize() : result;
  }
  bool verifyInstalled() { return recover() == DictionaryJournalResult::Ok && publication.verifyInstalled(); }
  DictionaryInstallationParent* installationParent() { return ready && parent.current() ? &parent : nullptr; }

 private:
  const Transfer& transfer;
  const TransferState& state;
  const ContentManifest& manifest;
  DictionaryExtractionJournal extractionJournal;
  DictionaryExtractionParent extraction;
  DictionaryInstallationJournal installationJournal;
  DictionaryInstallationParent parent;
  HalDictionaryPublicationSession publication;
  bool ready = false;
  DictionaryJournalResult fail(DictionaryJournalResult result, const char* operation) {
    ready = false;
    installationJournal.invalidate();
    extractionJournal.invalidate();
    if (result != DictionaryJournalResult::Missing)
      LOG_ERR("COMPANION", "Recovered dictionary %s failed: %u", operation, static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
