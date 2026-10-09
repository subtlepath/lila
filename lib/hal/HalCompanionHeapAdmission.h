#pragma once

#include <HalMemory.h>
#include <Logging.h>

namespace companion {
inline bool admitCompanionHeap(size_t bytes = 0, size_t largest = 0) {
  static constexpr size_t RESERVE = 50 * 1024;
  const auto heap = HalMemory::getInternalHeap();
  if (heap.freeBytes <= RESERVE || bytes >= heap.freeBytes - RESERVE || heap.largestBlockBytes < largest) {
    LOG_ERR("COMPANION", "Insufficient internal heap for companion operation");
    return false;
  }
  return true;
}
}  // namespace companion
