#pragma once

#include "CompanionDictionaryRetirementJournal.h"

namespace companion {
class DictionaryMemberJournalCleanupStorage : public DictionaryExtractionJournalStorage {
 public:
  // Check installed members, immutable archives and finalized binding under
  // exclusive ownership. Failure must preserve every journal.
  virtual bool verifyInstalled(const DictionaryInstallationPlan& proof) = 0;
  virtual bool remove(const char* path) = 0;
};
// Session-owned decoder/receipt state; all wire bytes use borrowed scratch.
// Retirement proofs and transfer journals remain intact throughout cleanup.
class DictionaryMemberJournalCleanup final {
 public:
  DictionaryMemberJournalCleanup(DictionaryRetirementJournal& retirement,
                                 DictionaryMemberJournalCleanupStorage& storage, std::span<uint8_t> scratch)
      : retirement(retirement), storage(storage), scratch(scratch) {}
  DictionaryJournalResult run() {
    auto proof = retirement.current();
    if (!proof || scratch.size() < DICTIONARY_INSTALLATION_PLAN_SIZE) return DictionaryJournalResult::Invalid;
    const auto recovered = retirement.recover(*proof);
    if (recovered != DictionaryJournalResult::Ok) return recovered;
    proof = retirement.current();
    if (!proof || !retirement.redundant()) return DictionaryJournalResult::Conflict;
    if (!storage.verifyInstalled(*proof)) return DictionaryJournalResult::IoError;
    present.fill(false);
    for (unsigned at = 0; at < 4; ++at) {
      uint64_t length = 0;
      const auto status = storage.stat(PATHS[at], length);
      if (retirement.current() != proof) return DictionaryJournalResult::Conflict;
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      const size_t expected = at < 2 ? DICTIONARY_EXTRACTION_RECEIPT_SIZE : DICTIONARY_INSTALLATION_PLAN_SIZE;
      if (length != expected) return DictionaryJournalResult::Corrupt;
      auto bytes = scratch.first(expected);
      if (!storage.read(PATHS[at], 0, bytes)) return DictionaryJournalResult::IoError;
      if (at < 2) {
        if (!decodeDictionaryExtractionReceipt(bytes, receipt) || !ownedReceipt(proof->extraction))
          return DictionaryJournalResult::Conflict;
      } else {
        const auto plan = codec.inspect(bytes);
        if (!plan || plan->extraction != proof->extraction || plan->archives != proof->archives ||
            plan->base != proof->base ||
            !(*plan == *proof || (plan->phase == DictionaryInstallationPhase::Bound && plan->revision != UINT64_MAX &&
                                  plan->revision + 1 == proof->revision)))
          return DictionaryJournalResult::Conflict;
      }
      present[at] = true;
    }
    for (unsigned at = 0; at < 4; ++at) {
      if (!present[at]) continue;
      if (retirement.current() != proof) return DictionaryJournalResult::Conflict;
      if (!storage.remove(PATHS[at])) return DictionaryJournalResult::IoError;
    }
    return DictionaryJournalResult::Ok;
  }

 private:
  static constexpr const char* PATHS[] = {DICTIONARY_EXTRACTION_JOURNALS[0], DICTIONARY_EXTRACTION_JOURNALS[1],
                                          DICTIONARY_INSTALLATION_JOURNALS[0], DICTIONARY_INSTALLATION_JOURNALS[1]};
  DictionaryRetirementJournal& retirement;
  DictionaryMemberJournalCleanupStorage& storage;
  std::span<uint8_t> scratch;
  DictionaryInstallationPlanCodec codec;
  DictionaryExtractionReceipt receipt;
  std::array<bool, 4> present{};
  bool ownedReceipt(const DictionaryExtractionReceipt& final) const {
    if (receipt == final) return true;
    const unsigned last = final.synonyms ? 3 : 0;
    if (receipt.revision == UINT64_MAX || receipt.revision + 1 != final.revision ||
        receipt.transaction != final.transaction || receipt.generation != final.generation ||
        receipt.archiveHash != final.archiveHash || receipt.lengths != final.lengths ||
        receipt.compressed != final.compressed || receipt.synonyms != final.synonyms ||
        receipt.sealed != (final.sealed & ~(1u << last)))
      return false;
    for (unsigned at = 0; at < 4; ++at)
      if (at == last ? dictionaryReceiptHashPresent(receipt.hashes[at]) : receipt.hashes[at] != final.hashes[at])
        return false;
    return true;
  }
};
}  // namespace companion
