#pragma once

#include <HalStorage.h>

#include "CompanionInventoryIndexBuilder.h"
#include "HalInventoryIndexStorage.h"
#include "HalInventoryRevisions.h"
#include "HalTransferStorage.h"

namespace companion {
// One serialized controller owns these paths; close catalog readers before use.
class HalInventoryIndexSink final : public InventoryIndexSink {
 public:
  HalInventoryIndexSink(const Identity& generation, std::span<uint8_t> scratch)
      : generation(generation), scratch(scratch) {}
  ~HalInventoryIndexSink() override;
  bool recover();
  bool buildAndPublish(SortedInventorySource& source, uint64_t& revision);
  bool nextRevision(uint64_t& revision);
  bool begin() override;
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override;
  bool finish(uint64_t bytes) override;
  void abort() override;
  static constexpr char ACTIVE[] = "/.crosspoint/companion/inventory";

 private:
  static constexpr char CANDIDATE[] = "/.crosspoint/companion/inventory-next";
  static constexpr char BACKUP[] = "/.crosspoint/companion/inventory-old";
  enum class Validation { Valid, Corrupt, IoError };
  Validation validate(const char* path, uint64_t* revision = nullptr);
  bool removeIfPresent(const char* path);
  bool closeCandidate();
  Identity generation;
  std::span<uint8_t> scratch;
  HalTransferStorage metadata;
  HalInventoryRevisions revisions;
  HalInventoryIndexStorage reader;
  HalFile candidate;
  bool writing = false;
  uint64_t reservedRevision = 0;
};
}  // namespace companion
