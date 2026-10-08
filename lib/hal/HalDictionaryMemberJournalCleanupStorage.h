#pragma once

#include "CompanionDictionaryMemberJournalCleanup.h"
#include "HalDictionaryExtractionJournalStorage.h"
#include "HalDictionaryInstalledVerification.h"

namespace companion {
// Reuses journal handles; the serialized retirement owner excludes SD writers.
class HalDictionaryMemberJournalCleanupStorage final : public DictionaryMemberJournalCleanupStorage {
 public:
  using OwnerGuard = HalDictionaryInstalledVerification::OwnerGuard;
  HalDictionaryMemberJournalCleanupStorage(DictionaryRetirementJournal& retirement,
                                           HalDictionaryInstalledVerification& installed,
                                           HalDictionaryExtractionJournalStorage& extraction,
                                           HalDictionaryExtractionJournalStorage& installation, OwnerGuard guard,
                                           void* owner)
      : retirement(retirement),
        installed(installed),
        extraction(extraction),
        installation(installation),
        guard(guard),
        owner(owner) {}
  bool verifyInstalled(const DictionaryInstallationPlan& proof) override {
    verified = false;
    const auto current = retirement.current();
    if (!current || !retirement.redundant() || *current != proof || !installed.verify(proof))
      return failure("installed proof");
    verified = true;
    if (!authorized()) {
      verified = false;
      return failure("ownership");
    }
    return true;
  }
  bool prepare() override { return authorized() || failure("prepare ownership"); }
  FileStatus stat(const char* path, uint64_t& length) override {
    auto provider = storageFor(path);
    if (!provider || !authorized()) {
      failure("stat arguments");
      return FileStatus::Error;
    }
    const auto status = provider->stat(path, length);
    if (!authorized()) {
      failure("stat ownership");
      return FileStatus::Error;
    }
    return status;
  }
  bool read(const char* path, uint64_t at, std::span<uint8_t> bytes) override {
    auto provider = storageFor(path);
    return (provider && authorized() && provider->read(path, at, bytes) && authorized()) || failure("read");
  }
  bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override { return failure("write prohibited"); }
  bool remove(const char* path) override {
    uint64_t length = 0;
    const auto presence = stat(path, length);
    if (presence == FileStatus::Error || !authorized()) return failure("remove lookup");
    if (presence == FileStatus::Missing) return true;
    if (!Storage.remove(path) || !authorized()) return failure("remove");
    return stat(path, length) == FileStatus::Missing || failure("remove readback");
  }

 private:
  DictionaryRetirementJournal& retirement;
  HalDictionaryInstalledVerification& installed;
  HalDictionaryExtractionJournalStorage& extraction;
  HalDictionaryExtractionJournalStorage& installation;
  OwnerGuard guard;
  void* owner;
  bool verified = false;
  bool authorized() const {
    const auto proof = retirement.current();
    return verified && proof && retirement.redundant() && guard && guard(owner, *proof);
  }
  HalDictionaryExtractionJournalStorage* storageFor(const char* path) {
    if (!path) return nullptr;
    for (const auto candidate : DICTIONARY_EXTRACTION_JOURNALS)
      if (std::strcmp(path, candidate) == 0) return &extraction;
    for (const auto candidate : DICTIONARY_INSTALLATION_JOURNALS)
      if (std::strcmp(path, candidate) == 0) return &installation;
    return nullptr;
  }
  static bool failure(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary member journal cleanup %s failed", reason);
    return false;
  }
};
}  // namespace companion
