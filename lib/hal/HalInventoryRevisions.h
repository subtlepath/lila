#pragma once
#include <cstdint>

namespace companion {
// Called only by the serialized companion controller; independent of SD rollback.
class HalInventoryRevisions {
 public:
  bool reserve(uint64_t after, uint64_t& revision);
};
}  // namespace companion
