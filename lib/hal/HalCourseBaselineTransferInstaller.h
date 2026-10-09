#pragma once

#include "CompanionTransfer.h"

namespace companion {
// Borrowed serialized owner: attach before recovery and detach before destruction.
class HalCourseBaselineTransferInstaller {
 public:
  virtual ~HalCourseBaselineTransferInstaller() = default;
  virtual bool prepare(const char* destination, const char* candidate, const ContentManifest& manifest,
                       const TransferState& state, std::span<uint8_t> scratch) = 0;
  virtual bool metadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                        std::span<uint8_t> scratch) = 0;
};
}  // namespace companion
