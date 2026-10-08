#pragma once

#include "HalDictionaryMemberJournalCleanupStorage.h"
#include "HalDictionaryRetirementCompletionStorage.h"

namespace companion {
// Retained coordinator state belongs outside the task stack. Providers, owner
// guard and wire scratch stay borrowed under exclusive retirement ownership.
class HalDictionaryRetirementSession final {
 public:
  HalDictionaryRetirementSession(DictionaryRetirementJournal& retirement, HalDictionaryInstalledVerification& installed,
                                 HalDictionaryExtractionJournalStorage& extraction,
                                 HalDictionaryExtractionJournalStorage& installation,
                                 HalDictionaryExtractionJournalStorage& proofs, HalCompanionFileLookup& lookup,
                                 HalDictionaryInstalledVerification::OwnerGuard guard, void* owner,
                                 std::span<uint8_t> scratch)
      : retirement(retirement),
        installed(installed),
        lookup(lookup),
        scratch(scratch),
        cleanupStorage(retirement, installed, extraction, installation, guard, owner),
        cleanup(retirement, cleanupStorage, scratch),
        completionStorage(retirement, installed, proofs, lookup, guard, owner),
        completion(retirement, completionStorage, scratch) {}
  DictionaryJournalResult begin(const DictionaryInstallationParent& parent) {
    const auto result = retirement.begin(parent);
    return result == DictionaryJournalResult::Ok ? result : fail(result, "proof publication");
  }
  DictionaryJournalResult resume() {
    auto result = retirement.recover();
    if (result != DictionaryJournalResult::Ok) return fail(result, "proof recovery");
    if (!retirement.redundant()) {
      result = retirement.repair(*retirement.current(), verify, this);
      if (result != DictionaryJournalResult::Ok) return fail(result, "proof repair");
    }
    result = discardIncoming();
    if (result != DictionaryJournalResult::Ok) return fail(result, "incoming stage");
    result = cleanup.run();
    if (result != DictionaryJournalResult::Ok) return fail(result, "member journals");
    result = completion.run();
    return result == DictionaryJournalResult::Ok ? result : fail(result, "completion");
  }

 private:
  DictionaryRetirementJournal& retirement;
  HalDictionaryInstalledVerification& installed;
  HalCompanionFileLookup& lookup;
  std::span<uint8_t> scratch;
  HalFile incoming;
  Digest incomingHash{};
  uint64_t incomingLength = 0;
  HalDictionaryMemberJournalCleanupStorage cleanupStorage;
  DictionaryMemberJournalCleanup cleanup;
  HalDictionaryRetirementCompletionStorage completionStorage;
  DictionaryRetirementCompletion completion;
  DictionaryJournalResult discardIncoming() {
    const auto proof = retirement.current();
    if (!proof || !retirement.redundant() || !installed.verify(*proof)) return DictionaryJournalResult::Conflict;
    const auto backup = lookup.inspect(TRANSFER_BACKUP);
    if (backup != CompanionFilePresence::Missing)
      return backup == CompanionFilePresence::Error ? DictionaryJournalResult::IoError
                                                    : DictionaryJournalResult::Conflict;
    const auto presence = lookup.inspect(TRANSFER_STAGE);
    if (presence == CompanionFilePresence::Missing) return DictionaryJournalResult::Ok;
    if (presence == CompanionFilePresence::Error ||
        !Storage.openFileForReadReusing("COMPANION", TRANSFER_STAGE, incoming))
      return DictionaryJournalResult::IoError;
    const bool sized = !incoming.isDirectory() && incoming.fileSize64() == proof->archives.original.length;
    const bool read = sized && hashInventoryFile(incoming, scratch, incomingLength, incomingHash);
    const bool closed = incoming.close();
    if (!closed) return DictionaryJournalResult::IoError;
    if (!sized) return DictionaryJournalResult::Corrupt;
    if (!read) return DictionaryJournalResult::IoError;
    if (incomingLength != proof->archives.original.length || incomingHash != proof->archives.original.contentHash)
      return DictionaryJournalResult::Corrupt;
    if (retirement.current() != proof || !Storage.remove(TRANSFER_STAGE)) return DictionaryJournalResult::IoError;
    return lookup.inspect(TRANSFER_STAGE) == CompanionFilePresence::Missing ? DictionaryJournalResult::Ok
                                                                            : DictionaryJournalResult::IoError;
  }
  static bool verify(void* context, const DictionaryInstallationPlan& proof) {
    return static_cast<HalDictionaryRetirementSession*>(context)->installed.verify(proof);
  }
  static DictionaryJournalResult fail(DictionaryJournalResult result, const char* operation) {
    if (result != DictionaryJournalResult::Missing)
      LOG_ERR("COMPANION", "Dictionary retirement session %s failed: %u", operation, static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
