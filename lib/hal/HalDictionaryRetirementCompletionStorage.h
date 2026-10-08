#pragma once

#include "CompanionDictionaryInstallationTemporaries.h"
#include "CompanionDictionaryRetirementCompletion.h"
#include "HalDictionaryExtractionJournalStorage.h"
#include "HalDictionaryInstalledVerification.h"

namespace companion {
// Borrowed providers; lookup handles are retained by the serialized session.
class HalDictionaryRetirementCompletionStorage final : public DictionaryRetirementCompletionStorage {
 public:
  using OwnerGuard = HalDictionaryInstalledVerification::OwnerGuard;
  HalDictionaryRetirementCompletionStorage(DictionaryRetirementJournal& retirement,
                                           HalDictionaryInstalledVerification& installed,
                                           HalDictionaryExtractionJournalStorage& proofs,
                                           HalCompanionFileLookup& lookup, OwnerGuard guard, void* owner)
      : retirement(retirement), installed(installed), proofs(proofs), lookup(lookup), guard(guard), owner(owner) {}
  bool verifyComplete(const DictionaryInstallationPlan& proof) override {
    verified = false;
    if (!retirement.current() || *retirement.current() != proof || !installed.verify(proof))
      return failure("installed verification");
    verified = true;
    for (const auto path : DICTIONARY_INSTALLATION_TEMPORARIES) {
      uint64_t length = 0;
      if (stat(path, length) != FileStatus::Missing) {
        verified = false;
        return failure("remaining temporary");
      }
    }
    return authorized() || failure("ownership");
  }
  FileStatus stat(const char* path, uint64_t& length) override {
    if (!authorized()) {
      failure("stat ownership");
      return FileStatus::Error;
    }
    if (isProof(path)) {
      const auto status = proofs.stat(path, length);
      return authorized() ? status : FileStatus::Error;
    }
    const auto presence = lookup.inspect(path);
    if (!authorized() || presence == CompanionFilePresence::Error) return FileStatus::Error;
    length = 0;
    return presence == CompanionFilePresence::Missing ? FileStatus::Missing : FileStatus::Present;
  }
  bool read(const char* path, std::span<uint8_t> bytes) override {
    return (isProof(path) && authorized() && proofs.read(path, 0, bytes) && authorized()) || failure("read");
  }
  bool remove(const char* path) override {
    if (!isProof(path) || !authorized()) return failure("remove arguments");
    uint64_t length = 0;
    const auto status = stat(path, length);
    if (status == FileStatus::Error) return failure("remove lookup");
    if (status == FileStatus::Missing) return true;
    if (!authorized() || !Storage.remove(path) || !authorized()) return failure("remove");
    return stat(path, length) == FileStatus::Missing || failure("remove readback");
  }

 private:
  DictionaryRetirementJournal& retirement;
  HalDictionaryInstalledVerification& installed;
  HalDictionaryExtractionJournalStorage& proofs;
  HalCompanionFileLookup& lookup;
  OwnerGuard guard;
  void* owner;
  bool verified = false;
  bool authorized() const {
    const auto proof = retirement.current();
    return verified && proof && guard && guard(owner, *proof);
  }
  static bool isProof(const char* path) {
    return path && (std::strcmp(path, DICTIONARY_RETIREMENT_JOURNALS[0]) == 0 ||
                    std::strcmp(path, DICTIONARY_RETIREMENT_JOURNALS[1]) == 0);
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary retirement completion %s failed", reason);
    return false;
  }
};
}  // namespace companion
