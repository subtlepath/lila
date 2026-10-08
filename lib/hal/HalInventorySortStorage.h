#pragma once

#include <HalStorage.h>

#include <array>

#include "CompanionInventorySorter.h"

namespace companion {
// Disposable files owned by one serialized inventory build session.
class HalInventorySortStorage final : public InventorySortStorage {
 public:
  // Paths are borrowed for the lifetime of the session.
  explicit HalInventorySortStorage(const char* first = RUN_PATHS[0], const char* second = RUN_PATHS[1])
      : runPaths{first, second} {}
  ~HalInventorySortStorage() override;
  bool reset() override;
  bool read(unsigned run, uint64_t offset, std::span<uint8_t> bytes) override;
  bool write(unsigned run, uint64_t offset, std::span<const uint8_t> bytes) override;
  bool finish(unsigned run, uint64_t size) override;
  bool close();
  static constexpr char RUN_PATHS[2][48] = {"/.crosspoint/companion/inventory-sort-a",
                                            "/.crosspoint/companion/inventory-sort-b"};

 private:
  void yieldPeriodically();
  bool failure(const char* operation);
  std::array<const char*, 2> runPaths;
  std::array<HalFile, 2> files;
  std::array<uint64_t, 2> lengths{};
  std::array<bool, 2> finished{};
  uint8_t operationsSinceYield = 0;
  bool failed = true;
};
}  // namespace companion
