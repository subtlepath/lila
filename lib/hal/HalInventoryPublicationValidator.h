#pragma once

#include "CompanionInventoryPairValidation.h"
#include "CompanionInventoryPublication.h"
#include "HalInventoryIndexStorage.h"

namespace companion {
// Session-owned; scratch outlives the validator and is borrowed by both readers.
class HalInventoryPublicationValidator final : public InventoryPublicationValidator {
 public:
  explicit HalInventoryPublicationValidator(std::span<uint8_t> scratch)
      : validation(indexStorage, pathStorage, scratch), scratch(scratch) {}
  InventoryValidation file(const char* path, bool paths, const Identity& generation, uint64_t revision) override;
  InventoryValidation pair(const char* index, const char* paths, const Identity& generation, uint64_t revision,
                           uint64_t* actualRevision = nullptr) override;

 private:
  HalInventoryIndexStorage indexStorage, pathStorage;
  InventoryPairValidation validation;
  std::span<uint8_t> scratch;
  bool close();
  InventoryValidation open(HalInventoryIndexStorage& reader, const char* path);
  InventoryValidation finish(bool valid);
  bool validateIndex(const Identity& generation, uint64_t revision);
  bool validatePaths(const Identity& generation, uint64_t revision);
};
}  // namespace companion
