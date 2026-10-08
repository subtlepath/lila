#pragma once

#include "CompanionDictionaryBundleBuilder.h"
#include "HalDictionaryZipExtraction.h"

namespace companion {
// Session-owned; the parent installer must keep staged files exclusively owned
// throughout validation and any subsequent canonical archive construction.
class HalDictionaryStagedSource final : public DictionaryBundleSource {
 public:
  ~HalDictionaryStagedSource() override { close(); }
  bool begin(const DictionaryZipMembers& members, const HalDictionaryZipExtraction& receipt, std::span<uint8_t> scratch,
             InventoryHashProgress progress = nullptr, void* context = nullptr) {
    if (!close()) return false;
    if (!receipt.isComplete() || scratch.size() < 64) return fail("Missing dictionary extraction receipt");
    count = members.hasSynonyms ? 4 : 3;
    reads = 0;
    lengths = {members.definitions.expandedBytes, members.index.expandedBytes, members.info.expandedBytes,
               members.synonyms.expandedBytes};
    for (unsigned at = 0; at < count; ++at) {
      const auto role = static_cast<HalZipEntryStage::Member>(at);
      const auto expected = receipt.contentHash(role);
      Digest actual{};
      uint64_t bytes = 0;
      if (!expected || !Storage.openFileForReadReusing("COMPANION", HalZipEntryStage::memberPath(role), files[at]) ||
          files[at].isDirectory() || !hashInventoryFile(files[at], scratch, bytes, actual, progress, context) ||
          bytes != lengths[at] || actual != *expected) {
        fail("Dictionary stage receipt verification failed");
        close();
        return false;
      }
    }
    ready = true;
    return true;
  }
  bool size(unsigned member, uint64_t& bytes) override {
    if (!usable(member)) return false;
    if (files[member].fileSize64() != lengths[member]) return fail("Dictionary stage length changed");
    bytes = lengths[member];
    return true;
  }
  bool read(unsigned member, uint64_t at, std::span<uint8_t> bytes) override {
    if (!usable(member) || bytes.size() > INT_MAX || at > lengths[member] || bytes.size() > lengths[member] - at)
      return fail("Dictionary stage read bounds failed");
    if (bytes.empty()) return true;
    if (!files[member].seek64(at) || files[member].read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()))
      return fail("Dictionary stage read failed");
    if (++reads == 32) {
      reads = 0;
      vTaskDelay(1);
    }
    return true;
  }
  bool close() override {
    ready = false;
    bool ok = true;
    for (unsigned at = files.size(); at > 0; --at)
      if (files[at - 1].isOpen() && !files[at - 1].close()) ok = false;
    return ok || fail("Dictionary stage close failed");
  }

 private:
  std::array<HalFile, 4> files;
  std::array<uint64_t, 4> lengths{};
  unsigned count = 0;
  uint8_t reads = 0;
  bool ready = false;
  bool usable(unsigned member) {
    return (ready && member < count && files[member].isOpen()) || fail("Dictionary stage source unavailable");
  }
  bool fail(const char* reason) {
    ready = false;
    LOG_ERR("COMPANION", "%s", reason);
    return false;
  }
};
}  // namespace companion
