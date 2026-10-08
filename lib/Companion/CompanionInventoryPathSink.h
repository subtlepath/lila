#pragma once

#include "CompanionRecords.h"

namespace companion {
class InventoryPathSink {
 public:
  virtual ~InventoryPathSink() = default;
  // Stage only; the owner publishes paths with their complete inventory index.
  virtual bool record(const ContentManifest& manifest, const char* path) = 0;
};
}  // namespace companion
