#pragma once

#include <HalStorage.h>

#include "CompanionInventoryIndex.h"

namespace companion {
class HalInventoryIndexStorage final : public InventoryIndexStorage {
 public:
  ~HalInventoryIndexStorage() override;
  bool open(const char* path);
  bool close();
  bool hadReadError() const { return failed; }
  bool size(uint64_t& bytes) override;
  bool read(uint64_t offset, std::span<uint8_t> output) override;

 private:
  HalFile file;
  uint64_t length = 0;
  uint8_t readsSinceYield = 0;
  bool failed = false;
};
}  // namespace companion
