#pragma once
#include "CompanionInventoryScan.h"
#include "HalInventoryRevisions.h"
namespace companion {
class HalInventoryRevisionAllocator final : public InventoryRevisionAllocator {
 public:
  bool reserve(uint64_t after, uint64_t& revision) override { return revisions.reserve(after, revision); }

 private:
  HalInventoryRevisions revisions;
};
}  // namespace companion
