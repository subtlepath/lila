#pragma once

#include "CompanionJournalIdentityIndex.h"
#include "HalInventoryIndexStorage.h"

namespace companion {
// Session-owned read handle; caller freezes the journal and the disposable index.
class HalJournalIdentityIndexStorage final : public JournalIdentityIndexStorage {
 public:
  bool open(const char* path = PATH) { return storage.open(path); }
  bool close() { return storage.close(); }
  bool hadReadError() const { return storage.hadReadError(); }
  bool size(uint64_t& bytes) override { return storage.size(bytes); }
  bool read(uint64_t offset, std::span<uint8_t> bytes) override { return storage.read(offset, bytes); }
  static constexpr char PATH[] = "/.crosspoint/companion/journal-identities";

 private:
  HalInventoryIndexStorage storage;
};
}  // namespace companion
