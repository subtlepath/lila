#pragma once
#include "CompanionInventoryIndexBuilder.h"
#include "HalInventorySnapshotStage.h"
namespace companion {
// Finish seals a private candidate; the pair coordinator publishes both files.
class HalInventoryIndexStage final : public InventoryIndexSink {
 public:
  static constexpr char CANDIDATE[] = "/.crosspoint/companion/inventory-next";
  bool begin() override { return stage.begin(); }
  bool write(uint64_t offset, std::span<const uint8_t> bytes) override { return stage.write(offset, bytes); }
  bool finish(uint64_t bytes) override { return stage.seal(bytes); }
  void abort() override { stage.abort(); }

 private:
  HalInventorySnapshotStage stage{HalInventorySnapshotStage::Kind::Index};
};
}  // namespace companion
