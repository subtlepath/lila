#pragma once

#include "CompanionDictionaryExtractionParent.h"
#include "HalCompanionFileLookup.h"
#include "HalZipEntryStage.h"

namespace companion {
// Use before member publication under exclusive parent ownership. The parent
// retains receipts until installation/rollback and all stage cleanup finish.
class HalDictionaryExtractionRecovery final {
 public:
  HalDictionaryExtractionRecovery(std::span<uint8_t> scratch, InventoryHashProgress progress = nullptr,
                                  void* context = nullptr)
      : lookup(progress, context), scratch(scratch), progress(progress), context(context) {}
  bool begin(DictionaryExtractionParent& parent, const DictionaryExtractionReceipt& initial) {
    if (!close() || !Storage.ready() || !Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) return fail("prepare");
    for (unsigned at = 0; at < 4; ++at)
      if (lookup.inspect(path(at)) != CompanionFilePresence::Missing) return fail("pre-existing stage");
    if (parent.begin(initial) != DictionaryJournalResult::Ok) return fail("initial receipt");
    return true;
  }
  bool verifyAndDiscardPartial(DictionaryExtractionParent& parent) {
    const auto receipt = parent.current();
    if (!receipt || scratch.size() < 64 || !close()) return fail("parent or scratch");
    present.fill(false);
    for (unsigned at = 0; at < 4; ++at) {
      if (!tick() || parent.current() != receipt) return fail("cancelled or changed parent");
      const auto presence = lookup.inspect(path(at));
      if (presence == CompanionFilePresence::Error) return fail("lookup");
      if (presence == CompanionFilePresence::Missing) {
        if (receipt->sealed & (1u << at)) return fail("missing sealed member");
        continue;
      }
      if (at == 3 && !receipt->synonyms) return fail("unexpected synonym stage");
      if (!Storage.openFileForReadReusing("COMPANION", path(at), file)) return fail("open");
      bool valid = !file.isDirectory() && file.fileSize64() <= receipt->lengths[at];
      if (valid && (receipt->sealed & (1u << at))) {
        Digest hash{};
        uint64_t bytes = 0;
        valid = hashInventoryFile(file, scratch, bytes, hash, progress, context) && bytes == receipt->lengths[at] &&
                hash == receipt->hashes[at];
      }
      const bool closed = close();
      if (!valid || !closed) return fail("stage verification");
      present[at] = true;
    }
    for (unsigned at = 0; at < 4; ++at) {
      if (!present[at] || (receipt->sealed & (1u << at))) continue;
      if (!tick() || parent.current() != receipt || !Storage.remove(path(at))) return fail("partial cleanup");
    }
    return true;
  }

 private:
  HalFile file;
  HalCompanionFileLookup lookup;
  std::span<uint8_t> scratch;
  InventoryHashProgress progress;
  void* context;
  std::array<bool, 4> present{};
  static const char* path(unsigned at) {
    return HalZipEntryStage::memberPath(static_cast<HalZipEntryStage::Member>(at));
  }
  bool tick() const { return !progress || progress(context); }
  bool close() { return !file.isOpen() || file.close() || fail("close"); }
  static bool fail(const char* operation) {
    LOG_ERR("COMPANION", "Dictionary stage recovery %s failed", operation);
    return false;
  }
};
}  // namespace companion
