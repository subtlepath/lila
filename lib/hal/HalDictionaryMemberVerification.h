#pragma once

#include "CompanionDictionaryInstallationPaths.h"
#include "CompanionDictionaryMemberPublication.h"
#include "HalZipEntryStage.h"

namespace companion {
// Caller has established presence and exclusive, unambiguous path ownership.
// Session-owned path and retained handle; an open failure never means missing.
class HalDictionaryMemberVerification final {
 public:
  explicit HalDictionaryMemberVerification(std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                                           void* context = nullptr)
      : scratch(scratch), progress(progress), context(context) {}
  ~HalDictionaryMemberVerification() { close(); }
  DictionaryMemberPresence verify(const DictionaryInstallationPlan& plan, unsigned member, bool installed) {
    if (!close() || scratch.size() < 64 || member >= 4 || !validDictionaryInstallationPlan(plan) ||
        (member == 3 && !plan.extraction.synonyms))
      return error("arguments or close");
    const char* selected = HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(member));
    if (installed) {
      if (!dictionaryInstallationMemberPath(plan, member, path)) return error("destination path");
      selected = path.data();
    }
    if (!Storage.openFileForReadReusing("COMPANION", selected, file)) return error("open");
    if (file.isDirectory() || file.fileSize64() != plan.extraction.lengths[member]) {
      if (!close()) return error("close conflicting member");
      LOG_ERR("COMPANION", "Dictionary member type or length mismatch");
      return DictionaryMemberPresence::Conflict;
    }
    Digest actual{};
    uint64_t length = 0;
    const bool hashed = hashInventoryFile(file, scratch, length, actual, progress, context);
    const bool closed = close();
    if (!hashed || !closed) return error("hash or close");
    if (length != plan.extraction.lengths[member] || actual != plan.extraction.hashes[member]) {
      LOG_ERR("COMPANION", "Dictionary member receipt mismatch");
      return DictionaryMemberPresence::Conflict;
    }
    return DictionaryMemberPresence::Verified;
  }

 private:
  HalFile file;
  std::array<char, DICTIONARY_INSTALLATION_MEMBER_PATH_CAPACITY> path{};
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  bool close() { return !file.isOpen() || file.close(); }
  static DictionaryMemberPresence error(const char* reason) {
    LOG_ERR("COMPANION", "Dictionary member verification %s failed", reason);
    return DictionaryMemberPresence::Error;
  }
};
}  // namespace companion
