#pragma once
#include "CompanionInventorySorter.h"
namespace companion {
class InventoryScan : public UnsortedInventorySource {
 public:
  // Begin a complete inventory scan; End never certifies a selected subset.
  virtual bool beginScan() = 0;
  virtual bool closeScan() = 0;
};
class InventoryRevisionAllocator {
 public:
  virtual ~InventoryRevisionAllocator() = default;
  virtual bool reserve(uint64_t after, uint64_t& revision) = 0;
};
}  // namespace companion
