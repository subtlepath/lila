#pragma once

#include "CompanionDictionaryRetirementJournal.h"

namespace companion {
class DictionaryRetirementCompletionStorage {
 public:
  virtual ~DictionaryRetirementCompletionStorage() = default;
  // Verify installed content and absence of owned staging under exclusive ownership.
  virtual bool verifyComplete(const DictionaryInstallationPlan& proof) = 0;
  virtual FileStatus stat(const char* path, uint64_t& length) = 0;
  virtual bool read(const char* path, std::span<uint8_t> bytes) = 0;
  virtual bool remove(const char* path) = 0;
};
// Borrowed wire scratch; decoder state remains in the session object.
class DictionaryRetirementCompletion final {
 public:
  DictionaryRetirementCompletion(DictionaryRetirementJournal& retirement,
                                 DictionaryRetirementCompletionStorage& storage, std::span<uint8_t> scratch)
      : retirement(retirement), storage(storage), scratch(scratch) {}
  DictionaryJournalResult run() {
    auto proof = retirement.current();
    if (!proof || scratch.size() < DICTIONARY_RETIREMENT_PROOF_SIZE) return DictionaryJournalResult::Invalid;
    const auto recovered = retirement.recover(*proof);
    if (recovered != DictionaryJournalResult::Ok) return recovered;
    proof = retirement.current();
    if (!proof || !storage.verifyComplete(*proof)) return DictionaryJournalResult::IoError;
    for (const auto paths : {DICTIONARY_EXTRACTION_JOURNALS, DICTIONARY_INSTALLATION_JOURNALS}) {
      for (unsigned slot = 0; slot < 2; ++slot) {
        uint64_t length = 0;
        const auto status = storage.stat(paths[slot], length);
        if (retirement.current() != proof) return DictionaryJournalResult::Conflict;
        if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
        if (status != FileStatus::Missing) return DictionaryJournalResult::Conflict;
      }
    }
    present.fill(false);
    auto bytes = scratch.first(DICTIONARY_RETIREMENT_PROOF_SIZE);
    for (unsigned slot = 0; slot < 2; ++slot) {
      uint64_t length = 0;
      const auto status = storage.stat(DICTIONARY_RETIREMENT_JOURNALS[slot], length);
      if (retirement.current() != proof) return DictionaryJournalResult::Conflict;
      if (status == FileStatus::Error) return DictionaryJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      if (length != bytes.size()) return DictionaryJournalResult::Corrupt;
      if (!storage.read(DICTIONARY_RETIREMENT_JOURNALS[slot], bytes)) return DictionaryJournalResult::IoError;
      const auto parsed = codec.inspect(bytes);
      if (!parsed || *parsed != *proof) return DictionaryJournalResult::Conflict;
      present[slot] = true;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
      if (!present[slot]) continue;
      if (retirement.current() != proof) return DictionaryJournalResult::Conflict;
      if (!storage.remove(DICTIONARY_RETIREMENT_JOURNALS[slot])) return DictionaryJournalResult::IoError;
    }
    retirement.invalidate();
    return DictionaryJournalResult::Ok;
  }

 private:
  DictionaryRetirementJournal& retirement;
  DictionaryRetirementCompletionStorage& storage;
  std::span<uint8_t> scratch;
  DictionaryRetirementProofCodec codec;
  std::array<bool, 2> present{};
};
}  // namespace companion
