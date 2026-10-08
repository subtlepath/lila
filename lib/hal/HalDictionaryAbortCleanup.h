#pragma once

#include <Memory.h>

#include "CompanionDictionaryInstallationTemporaries.h"
#include "HalDictionaryExtractionJournalStorage.h"
#include "HalDictionaryZipAudit.h"
#include "HalZipEntryStage.h"

namespace companion {
// Serialized Aborted owner, retained outside the stack. Receipts remain until
// all member/candidate stages and Prepared plan slots have been removed.
class HalDictionaryAbortCleanup final {
 public:
  HalDictionaryAbortCleanup(const Transfer& transfer, std::span<uint8_t> planScratch, std::span<uint8_t> wireScratch,
                            InventoryHashProgress progress = nullptr, void* context = nullptr)
      : transfer(transfer),
        receipts(extractionStorage, receiptScratch),
        plans(installationStorage, planScratch),
        scratch(wireScratch),
        progress(progress),
        context(context),
        lookup(progress, context) {}
  bool run() {
    const auto state = transfer.current();
    const auto manifest = transfer.contentManifest();
    if (!state || !manifest || state->phase != TransferPhase::Aborted || scratch.size() < 64 ||
        !validDictionaryBindingManifest(*manifest) || !matchesTransferManifest(*manifest, *state) ||
        !inventory_detail::nonzero(state->owner) || !inventory_detail::nonzero(state->transaction) ||
        !inventory_detail::nonzero(state->storageGeneration))
      return fail("parent context");
    const auto destination = transfer.destination();
    if (!validDictionaryInstallationBase(destination)) return fail("destination context");
    expected = *state;
    expectedManifest = *manifest;
    base.fill(0);
    std::copy(destination.begin(), destination.end(), base.begin());
    for (const auto path : DICTIONARY_RETIREMENT_JOURNALS)
      if (lookup.inspect(path) != CompanionFilePresence::Missing) return fail("unexpected retirement proof");
    if (lookup.inspect(TRANSFER_STAGE) != CompanionFilePresence::Missing ||
        lookup.inspect(TRANSFER_BACKUP) != CompanionFilePresence::Missing)
      return fail("incoming or backup remains");
    // The audit owner retains proof/lookup buffers beyond the task-local budget.
    auto audit = makeUniqueNoThrow<HalDictionaryZipAudit>(*state, *manifest, state->storageGeneration, destination,
                                                          scratch, progress, context);
    if (!audit) return fail("OOM: ZIP audit owner");
    if (!audit->abort() || !authorized()) return fail("ZIP audit cleanup");
    audit.reset();
    const auto result = receipts.recover(state->transaction, state->storageGeneration, state->contentHash);
    if (result == DictionaryJournalResult::Missing) return empty();
    if (result != DictionaryJournalResult::Ok || state->durableOffset != state->length)
      return fail("receipt ownership");
    const auto receipt = receipts.current();
    const auto plan = plans.recover(transfer);
    if (plan != DictionaryJournalResult::Missing &&
        (plan != DictionaryJournalResult::Ok || !plans.current() || plans.current()->extraction != *receipt ||
         plans.current()->phase != DictionaryInstallationPhase::Prepared))
      return fail("Prepared plan ownership");
    for (unsigned at = 0; at < 4; ++at) {
      present[at] = false;
      const auto presence = lookup.inspect(memberPath(at));
      if (presence == CompanionFilePresence::Error) return fail("member lookup");
      if (presence == CompanionFilePresence::Missing) continue;
      if ((at == 3 && !receipt->synonyms) || !Storage.openFileForReadReusing("COMPANION", memberPath(at), file))
        return fail("unexpected member or open");
      bool valid = !file.isDirectory() && file.fileSize64() <= receipt->lengths[at];
      if (valid && (receipt->sealed & (1u << at))) {
        valid = hashInventoryFile(file, scratch, length, hash, progress, context) && length == receipt->lengths[at] &&
                hash == receipt->hashes[at];
      }
      const bool closed = file.close();
      if (!valid || !closed) return fail("member proof");
      present[at] = true;
    }
    for (const auto path : {DICTIONARY_MEMBER_CANDIDATE, DICTIONARY_CACHE_CANDIDATE}) {
      const auto presence = lookup.inspect(path);
      if (presence == CompanionFilePresence::Error) return fail("candidate lookup");
      if (presence == CompanionFilePresence::Missing) continue;
      if (!Storage.openFileForReadReusing("COMPANION", path, file)) return fail("candidate open");
      const bool regular = !file.isDirectory() && file.fileSize64() <= UINT32_MAX;
      const bool closed = file.close();
      if (!regular || !closed) return fail("candidate type");
    }
    for (unsigned at = 0; at < 4; ++at)
      if (present[at] && !remove(memberPath(at))) return false;
    for (const auto path : {DICTIONARY_MEMBER_CANDIDATE, DICTIONARY_CACHE_CANDIDATE})
      if (!remove(path)) return false;
    for (const auto path : DICTIONARY_INSTALLATION_JOURNALS)
      if (!remove(path)) return false;
    for (const auto path : DICTIONARY_EXTRACTION_JOURNALS)
      if (!remove(path)) return false;
    return empty();
  }

 private:
  const Transfer& transfer;
  HalDictionaryExtractionJournalStorage extractionStorage;
  HalDictionaryExtractionJournalStorage installationStorage{
      HalDictionaryExtractionJournalStorage::Purpose::Installation};
  std::array<uint8_t, DICTIONARY_EXTRACTION_RECEIPT_SIZE> receiptScratch{};
  DictionaryExtractionJournal receipts;
  DictionaryInstallationJournal plans;
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  HalCompanionFileLookup lookup;
  HalFile file;
  std::array<bool, 4> present{};
  Digest hash{};
  uint64_t length = 0;
  TransferState expected;
  ContentManifest expectedManifest;
  std::array<char, 128> base{};
  static const char* memberPath(unsigned at) {
    return HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(at));
  }
  bool remove(const char* path) {
    if ((progress && !progress(context)) || !authorized()) return fail("changed owner or cancelled");
    const auto presence = lookup.inspect(path);
    if (presence == CompanionFilePresence::Missing) return true;
    if (presence != CompanionFilePresence::Present || !authorized() || !Storage.remove(path) || !authorized() ||
        lookup.inspect(path) != CompanionFilePresence::Missing)
      return fail("stage removal");
    return true;
  }
  bool empty() {
    for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) {
      if (!authorized()) return fail("changed owner");
      if (lookup.inspect(path) != CompanionFilePresence::Missing) return fail("remaining unowned temporary");
    }
    return authorized() || fail("changed owner");
  }
  bool authorized() const {
    return transfer.current() && *transfer.current() == expected && transfer.contentManifest() &&
           *transfer.contentManifest() == expectedManifest && transfer.destination() == std::string_view(base.data());
  }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary abort cleanup %s failed", operation);
    return false;
  }
};
}  // namespace companion
