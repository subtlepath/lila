#pragma once
#include "HalInventorySnapshotStage.h"
namespace companion {
class HalInventoryPathsStage final : public HalInventorySnapshotStage {
 public:
  static constexpr char CANDIDATE[] = "/.crosspoint/companion/inventory-paths-next";
  HalInventoryPathsStage() : HalInventorySnapshotStage(Kind::Paths) {}
};
}  // namespace companion
