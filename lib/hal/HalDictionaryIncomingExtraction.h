#pragma once

#include "CompanionDictionaryArchiveBinding.h"
#include "HalDictionaryArchiveSelection.h"
#include "HalDictionaryExtractionJournalStorage.h"
#include "HalDictionaryExtractionRecovery.h"
#include "HalDictionaryZipAudit.h"
#include "HalDictionaryZipExtraction.h"
#include "HalInventoryFileView.h"

namespace companion {
// Session-owned outside the task stack. File, parent state and scratch remain
// exclusively borrowed through preparation and subsequent canonical construction.
class HalDictionaryIncomingExtraction final {
 public:
  HalDictionaryIncomingExtraction(HalFile& file, const TransferState& state, const ContentManifest& manifest,
                                  const Identity& generation, std::string_view base, std::span<uint8_t> archiveScratch,
                                  std::span<uint8_t> comparisonScratch, std::span<uint8_t> receiptScratch,
                                  tinfl_decompressor* decoder, std::span<uint8_t> window,
                                  ZipNameWorkspace nameWorkspace, InventoryHashProgress progress = nullptr,
                                  void* context = nullptr)
      : file(file),
        state(state),
        manifest(manifest),
        generation(generation),
        scratch(comparisonScratch),
        progress(progress),
        context(context),
        audit(state, manifest, generation, base, comparisonScratch, progress, context),
        selector(source, archiveScratch, comparisonScratch, decoder, window, nameWorkspace, progress, context),
        journal(receiptStorage, receiptScratch),
        parent(journal, state, manifest, generation),
        recovery(comparisonScratch, progress, context),
        stage(comparisonScratch, progress, context),
        extractor(source, archiveScratch, decoder, window, progress, context),
        extraction(extractor, stage) {}
  bool prepare() {
    ready = false;
    if ((state.phase != TransferPhase::Receiving && state.phase != TransferPhase::Verified) ||
        state.storageGeneration != generation || state.durableOffset != state.length ||
        !validDictionaryBindingManifest(manifest) || !matchesTransferManifest(manifest, state) ||
        !hashInventoryFile(file, scratch, archiveLength, archiveHash, progress, context) ||
        archiveLength != manifest.length || archiveHash != manifest.contentHash || !source.attach(file) ||
        !audit.begin() || !selector.select(members) || !audit.finish())
      return fail("archive proof");
    initial = {};
    initial.revision = 1;
    initial.transaction = state.transaction;
    initial.generation = generation;
    initial.archiveHash = manifest.contentHash;
    initial.lengths = {members.definitions.expandedBytes, members.index.expandedBytes, members.info.expandedBytes,
                       members.hasSynonyms ? members.synonyms.expandedBytes : 0};
    initial.compressed = members.compressed;
    initial.synonyms = members.hasSynonyms;
    const auto result = parent.recover(initial);
    if (result == DictionaryJournalResult::Missing) {
      if (!recovery.begin(parent, initial)) return fail("initial ownership");
    } else if (result != DictionaryJournalResult::Ok)
      return fail("receipt recovery");
    if (!extraction.resume(members, parent, recovery)) return fail("member extraction");
    ready = true;
    return true;
  }
  const DictionaryZipMembers* selectedMembers() const { return ready && parent.current() ? &members : nullptr; }
  const HalDictionaryZipExtraction* extractedMembers() const {
    return ready && parent.current() ? &extraction : nullptr;
  }
  DictionaryExtractionParent* receiptParent() { return ready && parent.current() ? &parent : nullptr; }

 private:
  HalFile& file;
  const TransferState& state;
  const ContentManifest& manifest;
  const Identity& generation;
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  HalInventoryFileView source;
  HalDictionaryZipAudit audit;
  HalDictionaryArchiveSelection selector;
  HalDictionaryExtractionJournalStorage receiptStorage;
  DictionaryExtractionJournal journal;
  DictionaryExtractionParent parent;
  HalDictionaryExtractionRecovery recovery;
  HalZipEntryStage stage;
  ZipEntryExtraction extractor;
  HalDictionaryZipExtraction extraction;
  DictionaryZipMembers members;
  DictionaryExtractionReceipt initial;
  Digest archiveHash{};
  uint64_t archiveLength = 0;
  bool ready = false;
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Incoming dictionary %s failed", operation);
    return false;
  }
};
}  // namespace companion
