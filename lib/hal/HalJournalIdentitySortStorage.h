#pragma once

#include "CompanionJournalIdentitySorter.h"
#include "HalInventorySortStorage.h"

namespace companion {
// Session-owned; caller excludes concurrent journal-index builds.
class HalJournalIdentitySortStorage final : public JournalIdentitySortStorage {
 public:
  HalJournalIdentitySortStorage() : storage(RUN_PATHS[0], RUN_PATHS[1]) {}
  bool reset() override { return storage.reset(); }
  bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) override {
    return storage.read(run, offset, bytes);
  }
  bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) override {
    return storage.write(run, offset, bytes);
  }
  bool finish(unsigned run, uint64_t size) override { return storage.finish(run, size); }
  bool close() { return storage.close(); }
  static constexpr char RUN_PATHS[2][48] = {"/.crosspoint/companion/journal-sort-a",
                                            "/.crosspoint/companion/journal-sort-b"};

 private:
  HalInventorySortStorage storage;
};
}  // namespace companion
